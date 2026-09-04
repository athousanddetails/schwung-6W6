/*
 * sd606_plugin.cpp — Schwung plugin_api_v2 wrapper for the 6W6 engine.
 *
 * Runs in-process inside the shim's SPI callback (SCHED_FIFO 90). render_block
 * therefore does no allocation, no file I/O and takes no locks — all of that
 * happens in create_instance.
 *
 * Structure follows 9W9's er99_plugin.c on purpose: the two kits should feel
 * identical under the hands, so the pad map, silent-select window, per-lane
 * mutes and pad-follow are the same mechanisms with a 606
 * roster. GPL-3.0.
 */
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

    /* Pad-follow state. */
    int             focus_voice;
    unsigned        focus_count;

    /* Silent-select support: while samples_rendered < mute_until, the first
     * incoming note is swallowed. The editor sets this just before re-injecting
     * a Shift+Pad press into Move, so Move updates its pad selection while 6W6
     * stays quiet. */
    uint64_t        samples_rendered;
    uint64_t        mute_until;
    int             mute_one;
    float           last_bpm;

    /* Lane the on-device editor is showing (0-7, "8" = master). The
     * editor WRITES it on every pad-follow; the remote panel reads it back
     * through the manager's change push. It has to be a set_param because the
     * shim only notifies the manager about writes -- a value the DSP merely
     * computes (ui_focus_level) never reaches the browser. */
    char            ui_focus[8];
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
        /* Also the lane the remote panel follows. The editor overwrites this
         * with its own page when it is open; when it is not, this is what the
         * manager's periodic re-read of chain_params keys picks up. */
        snprintf(inst->ui_focus, sizeof(inst->ui_focus), "%d", v);
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
    if(!strcmp(_key, "ui_focus"))
    {
        snprintf(inst->ui_focus, sizeof(inst->ui_focus), "%s", _val);
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
        /* A blob saved before v1.6.0 carries a "seq_<lane>=<bits>;" tail after
         * the JSON. There is nothing left to apply it to, and sd606_deserialize
         * reads the JSON by key rather than by length, so the tail is simply
         * ignored and those patches still load. */
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
    /* ui_hierarchy is deliberately EMPTY, and empty is not the same as absent.
     *
     * enterComponentEdit prefers a module's hierarchy and only falls back to
     * loading the module's own ui_chain.js when there isn't one. 6W6 ships
     * ui_chain.js for the pad gestures, so it must declare no hierarchy...
     *
     * ...but it has to SAY so. The host's component load gate reads this key
     * and distinguishes THREE answers: JSON = declared, "" = served and empty
     * (fall back to ui_chain.js at once), null/error = the read did not
     * complete, hold and ask again. Returning -1 is the third, so Module ->
     * Swap -> 6W6 sat on the host's "Loading..." card forever. Answer with an
     * empty string and the module opens instantly. The normal entry path is
     * unchanged: getComponentHierarchy treats "" as "no hierarchy". */
    if(!strcmp(_key, "ui_hierarchy"))
    {
        if(_len < 1) return -1;
        _buf[0] = 0;
        return 0;
    }
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

    if(!strcmp(_key, "ui_focus"))
        return snprintf(_buf, (size_t)_len, "%s", inst->ui_focus[0] ? inst->ui_focus : "0");
    if(!strcmp(_key, "clock_running"))
    {
        const int clock = (g_host && g_host->get_clock_status)
                        ? g_host->get_clock_status() : MOVE_CLOCK_STATUS_STOPPED;
        return snprintf(_buf, (size_t)_len, "%d",
                        clock == MOVE_CLOCK_STATUS_RUNNING ? 1 : 0);
    }
    if(!strcmp(_key, "mutes"))
        return snprintf(_buf, (size_t)_len, "%u", sd606_get_mutes(inst->engine));
    if(!strcmp(_key, "state"))
    {
        return sd606_serialize(inst->engine, _buf, _len);
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

    /* Tempo for the synced delay, pushed in once per block and only when it
     * actually moves -- a param write is not free, and the BPM is constant
     * for thousands of blocks at a time. */
    if(g_host && g_host->get_bpm)
    {
        const float bpm = g_host->get_bpm();
        if(bpm > 20.0f && bpm != inst->last_bpm)
        {
            inst->last_bpm = bpm;
            char b[24];
            snprintf(b, sizeof(b), "%.4f", bpm);
            sd606_set_param(inst->engine, "dly_bpm", b);
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
