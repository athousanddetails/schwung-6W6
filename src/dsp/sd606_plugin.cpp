/*
 * sd606_plugin.cpp — Schwung plugin_api_v2 wrapper for the 6W6 engine.
 *
 * Runs in-process inside the shim's SPI callback (SCHED_FIFO 90). render_block
 * therefore does no allocation, no file I/O and takes no locks — all of that
 * happens in create_instance.
 *
 * Structure follows 9W9's er99_plugin.c on purpose: the two kits should feel
 * identical under the hands, so the pad map, silent-select window, per-lane
 * mutes, pad-follow and step sequencer are the same mechanisms with a 606
 * roster. GPL-3.0.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sd606_engine.h"
#include "sd606_params.h"
#include "plugin_api_v1.h"   /* defines both v1 and v2 */

static const host_api_v1_t *g_host = NULL;

/* Page ids from gen_params.py. Pad-follow publishes one of these. */
static const char *const kLevelOf[SD606_NUM_VOICES] = {
    "bd", "sd", "lt", "ht", "ch", "oh", "cy", "cp"
};

typedef struct {
    sd606_engine_t *engine;
    char            module_dir[512];

    /* Pad-follow state. Deliberately NOT updated while the transport runs —
     * otherwise every sequenced note yanks the editor to a different drum and
     * you can never keep a page open while a pattern plays. */
    int             focus_voice;
    unsigned        focus_count;

    /* ---- Per-voice step sequencer ----
     * 8 lanes x 16 steps, clocked from host get_beat_position() so it phase-
     * locks to whatever transport is running and stays drift-free. Step input
     * arrives as notes 16-31 through the patch's capture rules — the
     * Schwung-supported path, no core patching. */
    uint16_t        seq[SD606_NUM_VOICES];
    int             seq_voice;
    int             seq_last_step;

    /* Silent-select support: while samples_rendered < mute_until, the first
     * incoming note is swallowed. The editor sets this just before re-injecting
     * a Shift+Pad press into Move, so Move updates its pad selection while 6W6
     * stays quiet. */
    uint64_t        samples_rendered;
    uint64_t        mute_until;
    int             mute_one;
} sd606_instance_t;

/* ---- MIDI note map -------------------------------------------------- */
/*
 * Move's 32 pads are notes 68..99, laid out as 4 rows of 8, bottom-left first:
 *
 *   row 3 (top)    92 93 94 95 | 96 97 98 99
 *   row 2          84 85 86 87 | 88 89 90 91
 *   row 1          76 77 78 79 | 80 81 82 83
 *   row 0 (bottom) 68 69 70 71 | 72 73 74 75
 *                  ^---------^
 *                  drum block
 *
 * The kit lives in the LEFT block so it reads like a drum machine instead of
 * being smeared across the grid. Bottom row is the 606's front-panel order
 * (BD SD LT HT); the row above is the metal plus the clap.
 */
static int pad_to_voice(const uint8_t _note)
{
    switch(_note)
    {
    /* row 0 */
    case 68: return SD606_BD;   case 69: return SD606_SD;
    case 70: return SD606_LT;   case 71: return SD606_HT;
    /* row 1 */
    case 76: return SD606_CH;   case 77: return SD606_OH;
    case 78: return SD606_CY;   case 79: return SD606_CP;
    default: return -1;
    }
}

/*
 * Drum-rack map. On a Move DRUM track the pads do not send raw pad notes —
 * they send the drum rack's own notes from 36 (C1), bottom-left first, 4 per
 * row. Same kit order, so the physical layout is identical whichever way the
 * notes arrive. Default, because it makes each drum a separately sequencable
 * lane on a Move drum track.
 */
static int drumrack_to_voice(const uint8_t _note)
{
    if(_note >= 36 && _note <= 43) return (int)(_note - 36);
    return -1;
}

/* note_map: 0 = drum rack (default), 1 = General MIDI */
static int g_note_map = 0;

static int note_to_voice(const uint8_t _note)
{
    const int pad = pad_to_voice(_note);
    if(pad >= 0) return pad;

    if(g_note_map == 0) return drumrack_to_voice(_note);

    switch(_note)   /* GM drum map */
    {
    case 35: case 36: return SD606_BD;
    case 38: case 40: return SD606_SD;
    case 41: case 45: return SD606_LT;
    case 48: case 50: return SD606_HT;
    case 42: case 44: return SD606_CH;
    case 46:          return SD606_OH;
    case 49: case 51: case 57: return SD606_CY;
    case 39:          return SD606_CP;
    default:          return -1;
    }
}

/* ---- plugin_api_v2 -------------------------------------------------- */

static void *create_instance(const char *_module_dir, const char *_json_defaults)
{
    (void)_json_defaults;
    sd606_instance_t *inst = (sd606_instance_t *)calloc(1, sizeof(sd606_instance_t));
    if(!inst) return NULL;

    if(_module_dir)
        snprintf(inst->module_dir, sizeof(inst->module_dir), "%s", _module_dir);

    const float sr = (g_host && g_host->sample_rate > 0)
                   ? (float)g_host->sample_rate : 44100.0f;

    inst->engine = sd606_create(sr);
    if(!inst->engine) { free(inst); return NULL; }
    inst->seq_voice = 0;
    inst->seq_last_step = -1;

    if(g_host && g_host->log) g_host->log("6w6: engine ready");
    return inst;
}

static void destroy_instance(void *_instance)
{
    sd606_instance_t *inst = (sd606_instance_t *)_instance;
    if(!inst) return;
    sd606_destroy(inst->engine);
    free(inst);
}

static void on_midi(void *_instance, const uint8_t *_msg, const int _len, const int _source)
{
    sd606_instance_t *inst = (sd606_instance_t *)_instance;
    if(!inst || _len < 3) return;

    const uint8_t status = _msg[0] & 0xF0u;
    const uint8_t note   = _msg[1];
    const uint8_t vel    = _msg[2];

    /* Drums are one-shots: note-off is ignored, note-on with velocity 0 too. */
    if(status != 0x90 || vel == 0) return;

    /* Step buttons (notes 16-31): toggle the selected lane's step. These only
     * arrive while the patch's capture rules are active (slot focused), so
     * outside our editor the steps stay Move's. Internal surface only —
     * external gear sending 16-31 must not rewrite patterns. */
    if(note >= 16 && note <= 31 && _source == 0)
    {
        const int v = inst->seq_voice;
        if(v >= 0 && v < SD606_NUM_VOICES)
            inst->seq[v] ^= (uint16_t)(1u << (note - 16));
        return;
    }

    const int v = note_to_voice(note);
    if(v < 0) return;

    /* Silent-select window: swallow (only) the first note that arrives —
     * that is the Shift+Pad press routed back through Move. */
    if(inst->samples_rendered < inst->mute_until && inst->mute_one)
    {
        inst->mute_one = 0;
        return;
    }

    sd606_trigger(inst->engine, v, (int)vel);

    /* Only a hand-played hit moves the editor. While the transport runs, notes
     * arrive constantly and following them makes the UI unusable. */
    const int clock = (g_host && g_host->get_clock_status)
                    ? g_host->get_clock_status() : MOVE_CLOCK_STATUS_STOPPED;
    if(clock != MOVE_CLOCK_STATUS_RUNNING)
    {
        inst->focus_voice = v;
        inst->focus_count++;
    }
}

static void set_param(void *_instance, const char *_key, const char *_val)
{
    sd606_instance_t *inst = (sd606_instance_t *)_instance;
    if(!inst || !_key || !_val) return;

    if(!strcmp(_key, "note_map"))
    {
        g_note_map = atoi(_val) != 0;
        /* Also stored in the engine's enum table so it survives a state cycle. */
        sd606_set_param(inst->engine, _key, _val);
        return;
    }
    if(!strcmp(_key, "mute_ms"))
    {
        const int ms = atoi(_val);
        const float sr = (g_host && g_host->sample_rate > 0)
                       ? (float)g_host->sample_rate : 44100.0f;
        inst->mute_until = inst->samples_rendered
                         + (uint64_t)((ms > 0 ? ms : 0) * 0.001f * sr);
        inst->mute_one = 1;
        return;
    }
    if(!strcmp(_key, "mutes"))
    {
        sd606_set_mutes(inst->engine, (unsigned)atoi(_val));
        return;
    }
    if(!strcmp(_key, "seq_voice"))
    {
        const int v = atoi(_val);
        if(v >= 0 && v < SD606_NUM_VOICES) inst->seq_voice = v;
        return;
    }
    if(!strncmp(_key, "seq_", 4))
    {
        for(int v = 0; v < SD606_NUM_VOICES; ++v)
            if(!strcmp(_key + 4, kLevelOf[v]))
            { inst->seq[v] = (uint16_t)(atoi(_val) & 0xFFFF); return; }
    }
    /* Slot autosave and preset recall both arrive here. */
    if(!strcmp(_key, "state"))
    {
        sd606_deserialize(inst->engine, _val);
        /* note_map lives in the engine's enum table; mirror it back out to the
         * file-static the note router reads. */
        {
            char b[16];
            if(sd606_get_param(inst->engine, "note_map", b, sizeof(b)) > 0)
                g_note_map = atoi(b) != 0;
        }
        /* Plugin-level keys (sequencer lanes) piggyback on the same blob as a
         * "key=value;" tail after the JSON. */
        const char *q = _val;
        while((q = strstr(q, "seq_")) != NULL)
        {
            char kbuf[24];
            const char *eq = strchr(q, '=');
            const char *semi = strchr(q, ';');
            if(!eq || (semi && semi < eq)) { q += 4; continue; }
            const size_t kl = (size_t)(eq - q);
            if(kl < sizeof(kbuf))
            {
                memcpy(kbuf, q, kl); kbuf[kl] = '\0';
                set_param(_instance, kbuf, eq + 1);
            }
            q = eq + 1;
        }
        return;
    }
    sd606_set_param(inst->engine, _key, _val);
}

static int get_param(void *_instance, const char *_key, char *_buf, const int _len)
{
    sd606_instance_t *inst = (sd606_instance_t *)_instance;
    if(!inst || !_key || !_buf || _len <= 0) return -1;

    /* Served dynamically: module.json is capped at 8 KB by the loader and the
     * parameter surface is larger than that. */
    if(!strcmp(_key, "chain_params"))
    {
        if(_len <= SD606_CHAIN_PARAMS_LEN) return -1;   /* refuse to truncate */
        memcpy(_buf, sd606_chain_params_json, SD606_CHAIN_PARAMS_LEN + 1);
        return SD606_CHAIN_PARAMS_LEN;
    }
    /* ui_hierarchy is deliberately NOT served. enterComponentEdit prefers a
     * module's hierarchy and only falls back to loading the module's own
     * ui_chain.js when there isn't one. 6W6 ships ui_chain.js for the pad
     * gestures, so the hierarchy must stay absent here... */
    if(!strcmp(_key, "ui_hierarchy")) return -1;
    /* ...and is published under a key the host does not probe, for
     * ui_chain.js to feed the shared param_pages controller. */
    if(!strcmp(_key, "ui_pages"))
    {
        if(_len <= SD606_UI_PAGES_LEN) return -1;
        memcpy(_buf, sd606_ui_pages_json, SD606_UI_PAGES_LEN + 1);
        return SD606_UI_PAGES_LEN;
    }

    /*
     * Pad-follow. Publishes "<trigger-count>:<page-id>" so the editor can jump
     * to the drum you just hit. The counter is what the UI watches: it only
     * navigates on a NEW hit, so browsing to another page by hand is never
     * yanked away underneath you.
     */
    if(!strcmp(_key, "ui_focus_level"))
    {
        const int v = inst->focus_voice;
        if(inst->focus_count == 0 || v < 0 || v >= SD606_NUM_VOICES) return -1;
        return snprintf(_buf, (size_t)_len, "%u:%s", inst->focus_count, kLevelOf[v]);
    }

    if(!strcmp(_key, "clock_running"))
    {
        const int clock = (g_host && g_host->get_clock_status)
                        ? g_host->get_clock_status() : MOVE_CLOCK_STATUS_STOPPED;
        return snprintf(_buf, (size_t)_len, "%d",
                        clock == MOVE_CLOCK_STATUS_RUNNING ? 1 : 0);
    }
    if(!strcmp(_key, "mutes"))
        return snprintf(_buf, (size_t)_len, "%u", sd606_get_mutes(inst->engine));
    if(!strcmp(_key, "seq_voice"))
        return snprintf(_buf, (size_t)_len, "%d", inst->seq_voice);
    if(!strcmp(_key, "seq_pos"))
        return snprintf(_buf, (size_t)_len, "%d", inst->seq_last_step);
    if(!strncmp(_key, "seq_", 4))
    {
        for(int v = 0; v < SD606_NUM_VOICES; ++v)
            if(!strcmp(_key + 4, kLevelOf[v]))
                return snprintf(_buf, (size_t)_len, "%u", (unsigned)inst->seq[v]);
    }
    if(!strcmp(_key, "state"))
    {
        int n = sd606_serialize(inst->engine, _buf, _len);
        if(n < 0) return n;
        for(int v = 0; v < SD606_NUM_VOICES && n < _len - 1; ++v)
            n += snprintf(_buf + n, (size_t)(_len - n), "seq_%s=%u;",
                          kLevelOf[v], (unsigned)inst->seq[v]);
        return n < _len ? n : _len - 1;
    }

    return sd606_get_param(inst->engine, _key, _buf, _len);
}

static int get_error(void *_instance, char *_buf, const int _len)
{
    (void)_instance; (void)_buf; (void)_len;
    return 0;
}

static void render_block(void *_instance, int16_t *_out_lr, const int _frames)
{
    sd606_instance_t *inst = (sd606_instance_t *)_instance;
    if(!inst) { memset(_out_lr, 0, (size_t)_frames * 2 * sizeof(int16_t)); return; }

    inst->samples_rendered += (uint64_t)_frames;

    /* Advance the step sequencer. get_beat_position() < 0 or absent means no
     * transport — the sequencer idles and re-arms. 16th notes over one bar. */
    if(g_host && g_host->get_beat_position)
    {
        const double bp = g_host->get_beat_position();
        if(bp >= 0.0)
        {
            const int step = (int)floor(bp * 4.0) % 16;
            if(step != inst->seq_last_step)
            {
                inst->seq_last_step = step;
                for(int v = 0; v < SD606_NUM_VOICES; ++v)
                    if(inst->seq[v] & (1u << step))
                        sd606_trigger(inst->engine, v, 100);
            }
        }
        else
        {
            inst->seq_last_step = -1;
        }
    }

    /* Stack scratch: no allocation on the realtime path. Schwung's block is
     * 128 frames; guard anyway. */
    float mono[512];
    const int cap = (int)(sizeof(mono) / sizeof(mono[0]));
    int done = 0;
    while(done < _frames)
    {
        int chunk = _frames - done;
        if(chunk > cap) chunk = cap;

        sd606_render(inst->engine, mono, chunk);

        for(int i = 0; i < chunk; ++i)
        {
            float v = mono[i];
            if(v >  1.0f) v =  1.0f;
            if(v < -1.0f) v = -1.0f;
            const int16_t s = (int16_t)(v * 32767.0f);
            _out_lr[(done + i) * 2 + 0] = s;
            _out_lr[(done + i) * 2 + 1] = s;
        }
        done += chunk;
    }
}

static plugin_api_v2_t g_api;

extern "C" plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *_host)
{
    g_host = _host;
    /* Field-by-field, not a designated initialiser: the vendored voices pin us
     * to C++14 and designated initialisers are C++20. */
    g_api.api_version     = 2;
    g_api.create_instance = create_instance;
    g_api.destroy_instance= destroy_instance;
    g_api.on_midi         = on_midi;
    g_api.set_param       = set_param;
    g_api.get_param       = get_param;
    g_api.get_error       = get_error;
    g_api.render_block    = render_block;
    return &g_api;
}
