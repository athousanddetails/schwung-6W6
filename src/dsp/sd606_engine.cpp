/*
 * sd606_engine.cpp — see sd606_engine.h.
 *
 * GPL-3.0. The vendored voices under src/vendor/606 are MIT and unmodified.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "BassDrum.hpp"
#include "Clap.hpp"
#include "HiHats.hpp"
#include "Snare.hpp"
#include "Toms.hpp"

#include "sd606_cymbal.h"
#include "sd606_metal_hw.h"
#include "sd606_metal_voice.h"
#include "sd606_engine.h"
#include "sd606_params.h"
#include "sd606_shape.h"

using namespace SynthDrums606;

#define SD606_STATE_VERSION 1

/* A choke is a 2 ms fade, not a hard stop — cutting a ringing open hat dead
 * puts a click on the front of the closed hat that follows it. */
static const float kChokeSeconds = 0.002f;


namespace {

const char *kVoiceIds[SD606_NUM_VOICES] = {
    "bd", "sd", "lt", "ht", "ch", "oh", "cy", "cp"
};

/*
 * Kit balance. Left to themselves the vendored voices each arrive at roughly
 * full scale — every one of them was fitted against a solo hardware recording,
 * so eight at unity is 8x too loud and the mix clips before a knob is touched.
 *
 * These trims put the kit in proportion at pot centre, so Level 64 means "the
 * balanced 606", not "unity gain". Headroom is set so an ACCENTED kick (2x by
 * default) still lands under full scale rather than on top of it.
 *
 * Measured, not guessed: solo peaks at unity were bd 0.672, sd 0.847, lt 0.869,
 * ht 0.879, ch 0.920, oh 1.089, cy 0.980, cp 0.608; re-measured after the default pots were fitted against the hardware kit
 * (the kick especially: short and clicky now, not the XL tail).
 */
static const float kVoiceTrim[SD606_NUM_VOICES] = {
    0.751f,   /* bd — loudest of the kit, as on the hardware */
    0.446f,   /* sd */
    0.308f,   /* lt */
    0.309f,   /* ht */
    0.183f,   /* ch */
    0.251f,   /* oh */
    0.203f,   /* cy — re-measured after the cymbal was fitted */
    0.460f,   /* cp */
};

/* Per-voice pot/enum slots, resolved once at create time so the audio path
 * never searches by string. */
struct VoiceSlots {
    int tune, decay, drive, level;   /* every voice has these four */
    int dist;                        /* enum slot                  */
};

struct VoiceRt {
    float hit_gain;      /* this hit's accent scale        */
    float choke_gain;    /* 1.0 normally, ramps to 0 on a choke */
    float choke_step;    /* < 0 while choking, else 0      */
};

int find_pot(const char *key)
{
    for(int i = 0; i < SD606_NUM_POTS; ++i)
        if(!strcmp(g_sd606_pots[i].key, key)) return i;
    return -1;
}

int find_enum(const char *key)
{
    for(int i = 0; i < SD606_NUM_ENUMS; ++i)
        if(!strcmp(g_sd606_enums[i].key, key)) return i;
    return -1;
}

/* pot position 0..127 -> engineering value.
 * EXP: value = min * (max/min)^(pot/127). Fine control at the bottom of a
 * time or frequency range, where the ear actually is. */
float pot_value(int slot, int pot)
{
    const sd606_pot_t &p = g_sd606_pots[slot];
    const float t = (float)pot / 127.0f;
    if(p.curve == SD606_EXP && p.min > 0.0f)
        return p.min * powf(p.max / p.min, t);
    return p.min + (p.max - p.min) * t;
}

} /* namespace */

struct sd606_engine {
    float sample_rate;

    int   pot[SD606_NUM_POTS];       /* raw 0..127, the stored form */
    float potv[SD606_NUM_POTS];      /* resolved, recomputed on write */
    int   env[SD606_NUM_ENUMS];      /* enum selections */

    VoiceSlots slot[SD606_NUM_VOICES];
    VoiceRt    rt[SD606_NUM_VOICES];

    /* Voice-specific extras that only some lanes have. */
    int bd_attack, bd_drift, sd_snappy, sd_tone, cp_noise;
    /* Globals. */
    int e_master_dist, e_choke, e_note_map;
    int p_master_drive, p_volume, p_accent;

    unsigned mutes;
    unsigned rng;                    /* per-hit kick drift */

    BassDrumVoice   bd;
    SnareVoice      sd;
    TomVoice        lt, ht;
    Sd606MetalVoice ch, oh, cy;   /* forked: see sd606_metal_voice.h */
    ClapVoice       cp;
};

const char *sd606_voice_id(int voice)
{
    return (voice >= 0 && voice < SD606_NUM_VOICES) ? kVoiceIds[voice] : "";
}

sd606_engine_t *sd606_create(float sample_rate)
{
    sd606_engine_t *e = (sd606_engine_t *)calloc(1, sizeof(sd606_engine_t));
    if(!e) return NULL;
    e->sample_rate = sample_rate > 0.0f ? sample_rate : 44100.0f;
    e->rng = 0x9E3779B9u;

    for(int i = 0; i < SD606_NUM_POTS; ++i)
    {
        e->pot[i]  = g_sd606_pots[i].def;
        e->potv[i] = pot_value(i, e->pot[i]);
    }
    for(int i = 0; i < SD606_NUM_ENUMS; ++i)
        e->env[i] = g_sd606_enums[i].def;

    /* Resolve every key once. A miss here is a generator/engine mismatch and
     * the loadtest asserts on it rather than letting it degrade silently. */
    char key[64];
    for(int v = 0; v < SD606_NUM_VOICES; ++v)
    {
        const char *id = kVoiceIds[v];
        snprintf(key, sizeof(key), "%s_tune",      id); e->slot[v].tune  = find_pot(key);
        snprintf(key, sizeof(key), "%s_decay",     id); e->slot[v].decay = find_pot(key);
        snprintf(key, sizeof(key), "%s_drive",     id); e->slot[v].drive = find_pot(key);
        snprintf(key, sizeof(key), "%s_level",     id); e->slot[v].level = find_pot(key);
        snprintf(key, sizeof(key), "%s_dist_type", id); e->slot[v].dist  = find_enum(key);
        e->rt[v].hit_gain   = 1.0f;
        e->rt[v].choke_gain = 1.0f;
        e->rt[v].choke_step = 0.0f;
    }
    e->bd_attack = find_pot("bd_attack");
    e->bd_drift  = find_pot("bd_drift");
    e->sd_snappy = find_pot("sd_snappy");
    e->sd_tone   = find_pot("sd_tone");
    e->cp_noise  = find_pot("cp_noise");
    e->p_master_drive = find_pot("master_drive");
    e->p_volume       = find_pot("volume");
    e->p_accent       = find_pot("accent");
    e->e_master_dist  = find_enum("master_dist");
    e->e_choke        = find_enum("hh_choke");
    e->e_note_map     = find_enum("note_map");

    const double sr = e->sample_rate;
    /* Distinct seeds so the per-hit analog variation of one voice does not
     * shuffle in lockstep with another's. */
    e->bd.init(sr, 0x6060u);
    e->sd.init(sr, 0x6063u);
    e->lt.init(sr, 0x6061u);
    e->ht.init(sr, 0x6062u);
    e->ch.init(sr, 0x6064u);
    e->oh.init(sr, 0x6065u);
    e->cy.init(sr, 0x6066u);
    e->cp.init(sr, 0x0606C1A9u);
    return e;
}

void sd606_destroy(sd606_engine_t *e) { free(e); }

void sd606_set_mutes(sd606_engine_t *e, unsigned mask)
{
    e->mutes = mask & ((1u << SD606_NUM_VOICES) - 1u);
    for(int v = 0; v < SD606_NUM_VOICES; ++v)
    {
        if(e->mutes & (1u << v))
        {
            /* Fade rather than cut, same reason as the choke. The lane keeps
             * rendering through the ramp; it drops out of the mix only once
             * the gain reaches zero. */
            if(e->rt[v].choke_gain > 0.0f && e->rt[v].choke_step == 0.0f)
                e->rt[v].choke_step = -1.0f / (kChokeSeconds * e->sample_rate);
        }
        else if(e->rt[v].choke_step < 0.0f)
        {
            /* Unmuted mid-fade: stop fading, but do not resurrect the tail —
             * the lane comes back on its next hit. */
            e->rt[v].choke_step = 0.0f;
        }
    }
}

unsigned sd606_get_mutes(const sd606_engine_t *e) { return e->mutes; }

static void choke_voice(sd606_engine_t *e, int v)
{
    if(e->rt[v].choke_gain > 0.0f && e->rt[v].choke_step == 0.0f)
        e->rt[v].choke_step = -1.0f / (kChokeSeconds * e->sample_rate);
}

void sd606_trigger(sd606_engine_t *e, int voice, int velocity)
{
    if(voice < 0 || voice >= SD606_NUM_VOICES) return;
    if(velocity <= 0) return;                       /* note-off: drums are one-shots */
    if(e->mutes & (1u << voice)) return;

    const float accent = velocity >= SD606_ACCENT_VELOCITY
                       ? e->potv[e->p_accent] : 1.0f;
    e->rt[voice].hit_gain   = accent;
    e->rt[voice].choke_gain = 1.0f;
    e->rt[voice].choke_step = 0.0f;

    const VoiceSlots &s = e->slot[voice];
    const float decay = e->potv[s.decay];
    const float tune  = e->potv[s.tune];

    /* The 606 hardwires "closed hat cuts open hat" because they share one
     * circuit. Here it is a switch: Off / CH > OH / Mutual. The cymbal is
     * deliberately outside the group — on the hardware it shares the metal
     * bank, but nobody wants a hi-hat swallowing their crash. */
    const int choke = e->env[e->e_choke];
    if(voice == SD606_CH && choke >= 1) choke_voice(e, SD606_OH);
    if(voice == SD606_OH && choke == 2) choke_voice(e, SD606_CH);

    switch(voice)
    {
    case SD606_BD: {
        /* Drift is per-hit pitch jitter in semitones — the thing that stops a
         * programmed 606 kick sounding like a copy-paste of itself. */
        e->rng = e->rng * 1664525u + 1013904223u;
        const float unit  = (float)((e->rng >> 8) & 0xFFFF) / 32767.5f - 1.0f;
        const float jitter = unit * e->potv[e->bd_drift] * 1.5f;
        e->bd.trigger(e->potv[e->bd_attack], decay, tune, jitter);
        break;
    }
    case SD606_SD:
        e->sd.trigger(decay, tune, e->potv[e->sd_snappy], e->potv[e->sd_tone]);
        break;
    case SD606_LT: e->lt.trigger(kLowTomSpec,  decay, tune); break;
    case SD606_HT: e->ht.trigger(kHighTomSpec, decay, tune); break;
    case SD606_CH: e->ch.trigger(kHwClosedHatSpec, decay, tune); break;   /* the owner's 606, see sd606_metal_hw.h */
    case SD606_OH: e->oh.trigger(kHwOpenHatSpec,   decay, tune); break;
    case SD606_CY: e->cy.trigger(kCymbalSpec,    decay, tune); break;
    case SD606_CP: e->cp.trigger(decay, tune, e->potv[e->cp_noise]); break;
    default: break;
    }
}

/* One sample from one lane, through its own drive stage. */
static inline float voice_sample(sd606_engine *e, int v, float raw)
{
    VoiceRt &r = e->rt[v];
    if(r.choke_step < 0.0f)
    {
        r.choke_gain += r.choke_step;
        if(r.choke_gain <= 0.0f) { r.choke_gain = 0.0f; r.choke_step = 0.0f; }
    }
    if(r.choke_gain <= 0.0f) return 0.0f;

    const VoiceSlots &s = e->slot[v];
    const float shaped = sd606_shape(raw, e->potv[s.drive], e->env[s.dist]);
    return shaped * e->potv[s.level] * kVoiceTrim[v] * r.hit_gain * r.choke_gain;
}

/* A silent lane must not cost a process() call — the metal voices sum 47
 * sines each and three of them can be ringing at once. Guarding on the gate
 * gain also means a muted lane keeps rendering until its fade completes, then
 * disappears. */
#define SD606_LANE(vid, expr) \
    do { if(e->rt[vid].choke_gain > 0.0f) mix += voice_sample(e, vid, (expr)); } while(0)

void sd606_render(sd606_engine_t *e, float *out, int frames)
{
    const int   mdist  = e->env[e->e_master_dist];
    const float mdrive = e->potv[e->p_master_drive];
    const float vol    = e->potv[e->p_volume];

    for(int i = 0; i < frames; ++i)
    {
        float mix = 0.0f;
        SD606_LANE(SD606_BD, e->bd.process());
        SD606_LANE(SD606_SD, e->sd.process());
        SD606_LANE(SD606_LT, e->lt.process());
        SD606_LANE(SD606_HT, e->ht.process());
        SD606_LANE(SD606_CH, e->ch.process());
        SD606_LANE(SD606_OH, e->oh.process());
        SD606_LANE(SD606_CY, e->cy.process());
        SD606_LANE(SD606_CP, e->cp.process());

        /* Master stage. Option 0 is Off, so the kit can be left alone. */
        if(mdist > 0) mix = sd606_shape(mix, mdrive, mdist - 1);
        mix *= vol;

        if(!(mix > -8.0f && mix < 8.0f)) mix = 0.0f;   /* also catches NaN */
        out[i] = mix;
    }
}

/* ---- parameters ------------------------------------------------------- */

int sd606_set_param(sd606_engine_t *e, const char *key, const char *val)
{
    const int slot = find_pot(key);
    if(slot >= 0)
    {
        int p = (int)(atof(val) + 0.5f);
        if(p < 0) p = 0;
        if(p > 127) p = 127;                 /* clamp, never wrap */
        e->pot[slot]  = p;
        e->potv[slot] = pot_value(slot, p);
        return 1;
    }
    const int es = find_enum(key);
    if(es >= 0)
    {
        int v = (int)(atof(val) + 0.5f);
        if(v < 0) v = 0;
        if(v >= g_sd606_enums[es].count) v = g_sd606_enums[es].count - 1;
        e->env[es] = v;
        return 1;
    }
    return 0;
}

int sd606_get_param(sd606_engine_t *e, const char *key, char *buf, int len)
{
    const int slot = find_pot(key);
    if(slot >= 0) return snprintf(buf, len, "%d", e->pot[slot]);
    const int es = find_enum(key);
    if(es >= 0)   return snprintf(buf, len, "%d", e->env[es]);
    return -1;
}

int sd606_serialize(const sd606_engine_t *e, char *buf, int len)
{
    int n = snprintf(buf, len, "{\"v\":%d,\"pots\":[", SD606_STATE_VERSION);
    for(int i = 0; i < SD606_NUM_POTS && n < len; ++i)
        n += snprintf(buf + n, len - n, i ? ",%d" : "%d", e->pot[i]);
    if(n < len) n += snprintf(buf + n, len - n, "],\"enums\":[");
    for(int i = 0; i < SD606_NUM_ENUMS && n < len; ++i)
        n += snprintf(buf + n, len - n, i ? ",%d" : "%d", e->env[i]);
    if(n < len) n += snprintf(buf + n, len - n, "],\"mutes\":%u}", e->mutes);
    return n;
}

/* Reads the arrays positionally. A blob shorter than the current table is a
 * patch saved before a control was appended — the missing tail keeps its
 * default rather than reading garbage. */
static const char *scan_ints(const char *p, int *dst, int max, int *got)
{
    *got = 0;
    if(!p) return NULL;
    p = strchr(p, '[');
    if(!p) return NULL;
    ++p;
    while(*p && *p != ']' && *got < max)
    {
        while(*p == ' ' || *p == ',') ++p;
        if(*p == ']' || !*p) break;
        char *end = NULL;
        dst[(*got)++] = (int)strtol(p, &end, 10);
        if(end == p) break;      /* not a number: stop, do not spin */
        p = end;
    }
    const char *end = strchr(p, ']');
    return end ? end + 1 : NULL;
}

void sd606_deserialize(sd606_engine_t *e, const char *json)
{
    if(!json || !*json) return;
    const char *p = strstr(json, "\"pots\"");
    int vals[SD606_NUM_POTS > SD606_NUM_ENUMS ? SD606_NUM_POTS : SD606_NUM_ENUMS];
    int got = 0;

    p = scan_ints(p, vals, SD606_NUM_POTS, &got);
    for(int i = 0; i < got; ++i)
    {
        int v = vals[i] < 0 ? 0 : (vals[i] > 127 ? 127 : vals[i]);
        e->pot[i]  = v;
        e->potv[i] = pot_value(i, v);
    }

    const char *q = strstr(json, "\"enums\"");
    scan_ints(q, vals, SD606_NUM_ENUMS, &got);
    for(int i = 0; i < got; ++i)
    {
        int v = vals[i] < 0 ? 0 : vals[i];
        if(v >= g_sd606_enums[i].count) v = g_sd606_enums[i].count - 1;
        e->env[i] = v;
    }

    const char *mp = strstr(json, "\"mutes\"");
    if(mp) { mp = strchr(mp, ':'); if(mp) e->mutes = (unsigned)strtoul(mp + 1, NULL, 10)
                                                     & ((1u << SD606_NUM_VOICES) - 1u); }
}

/* Read-only view of the kit balance, for the trim-measurement probe in
 * tools/. Not part of the plugin surface. */
extern "C" const float *sd606_debug_trim(void) { return kVoiceTrim; }
