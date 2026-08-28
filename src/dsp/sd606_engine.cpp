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
#include "sd606_clap_voice.h"
#include "sd606_fx.h"
#include "sd606_metal_voice.h"
#include "sd606_engine.h"
#include "sd606_params.h"
#include "sd606_shape.h"

using namespace SynthDrums606;

/* 2: the drive pots changed range (0.2,8)->(0.85,12) and the distortion enums
 * grew 4->7 / 5->8 with Fold and Crush moving. Both silently change what a
 * STORED value means -- the blob is positional and stores raw pot positions --
 * so v1 blobs are migrated on load. See migrate_v1(). */
/* 3: the Accent pot is gone -- velocity subsumed it -- which renumbers
 * everything after it. v1 and v2 blobs are both loaded by NAME. */
#define SD606_STATE_VERSION 3

/* The snare's decay, fitted against the hardware and then pinned: the machine
 * has no such control. 76/127 of the old (0,1) pot. */
#define SD606_SD_DECAY_FIXED 0.5984252f

/*
 * The gain a FULL-velocity hit reaches. This is the old Accent pot's default
 * (1 + 42/127 * 3), kept as a constant when the control was removed.
 *
 * The number matters. Accent was never "extra" -- it was the level a pattern
 * from Move actually played at, because Move sends velocity 100 and up. Drop
 * the pot and anchor the velocity line at 1.0 instead and the whole kit comes
 * back 6 dB quieter, which is the trap 9W9 caught before shipping. Anchoring
 * here keeps every existing pattern at exactly the level it had.
 */
#define SD606_FULL_VELOCITY_GAIN 1.9921260f

/*
 * Drive at the bottom of the knob must be TRANSPARENT, and with a (0.85, 12)
 * range it is not: pot 0 is drive 0.85, and diode rounding at 0.85 still
 * shapes and still adds about +1.8 dB of small-signal gain. Widening the
 * range cannot fix that -- even drive 1.0 is tanh(x)/tanh(1), not identity.
 *
 * So the bottom of the knob crossfades dry into the shaped signal instead:
 * pot 0 is EXACTLY the dry sample (the shaper is not even called) and the
 * blend reaches fully wet at pot 8, which is the default. Above 8 nothing
 * changes at all -- that is deliberate, and it is what keeps the fitted kit
 * bit-identical at its defaults.
 */
#define SD606_DRIVE_WET_POT 8

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
    int rev, dly;                    /* post-fader send amounts    */
};

struct VoiceRt {
    float hit_gain;      /* this hit's accent scale        */
    float choke_gain;    /* 1.0 normally, ramps to 0 on a choke */
    float choke_step;    /* < 0 while choking, else 0      */
    /* Crush decimator: held sample + phase. Only distortion type 6 touches
     * it. Reset on trigger, so a hit never inherits the previous one's held
     * sample. */
    float crush_st[2];
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
    int bd_attack, sd_snappy, sd_tone, cp_noise;
    /* Globals. */
    int e_master_dist, e_choke, e_note_map;
    int p_master_drive, p_volume, p_vel_depth;

    float crush_master[2];           /* master-stage crush decimator */

    /* Send buses. Big buffers (the delay line alone is 88200 floats), which
     * is why the engine is heap-allocated in sd606_create and never on a
     * stack anywhere near the audio path. */
    sd606_verb_t verb;
    sd606_dly_t  dly;
    sd606_glue_t glue;
    int p_comp;
    int p_rev_decay, p_rev_tone, p_rev_hpf, p_rev_level;
    int p_dly_fdbk, p_dly_tone, p_dly_hpf, p_dly_level;
    int e_dly_time;
    unsigned mutes;
    unsigned rng;                    /* per-hit kick drift */

    BassDrumVoice   bd;
    SnareVoice      sd;
    TomVoice        lt, ht;
    Sd606MetalVoice ch, oh, cy;   /* forked: see sd606_metal_voice.h */
    Sd606ClapVoice  cp;   /* forked: see sd606_clap_voice.h */
};

static void sd606_fx_sync(sd606_engine_t *e);

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
        /* SD has no decay pot any more -- see SD606_SD_DECAY_FIXED. Point it
         * at a valid slot so nothing ever indexes -1; the value is unused. */
        if(e->slot[v].decay < 0) e->slot[v].decay = e->slot[v].tune;
        snprintf(key, sizeof(key), "%s_drive",     id); e->slot[v].drive = find_pot(key);
        snprintf(key, sizeof(key), "%s_level",     id); e->slot[v].level = find_pot(key);
        snprintf(key, sizeof(key), "%s_dist_type", id); e->slot[v].dist  = find_enum(key);
        snprintf(key, sizeof(key), "%s_rev",       id); e->slot[v].rev   = find_pot(key);
        snprintf(key, sizeof(key), "%s_dly",       id); e->slot[v].dly   = find_pot(key);
        e->rt[v].hit_gain   = 1.0f;
        e->rt[v].choke_gain = 1.0f;
        e->rt[v].choke_step = 0.0f;
    }
    e->bd_attack = find_pot("bd_attack");
    e->sd_snappy = find_pot("sd_snappy");
    e->sd_tone   = find_pot("sd_tone");
    e->cp_noise  = find_pot("cp_noise");
    e->p_master_drive = find_pot("master_drive");
    e->p_volume       = find_pot("volume");
    e->p_vel_depth    = find_pot("vel_depth");
    e->e_master_dist  = find_enum("master_dist");
    e->e_choke        = find_enum("hh_choke");
    e->e_note_map     = find_enum("note_map");
    e->p_rev_decay = find_pot("rev_decay"); e->p_rev_tone  = find_pot("rev_tone");
    e->p_rev_hpf   = find_pot("rev_hpf");   e->p_rev_level = find_pot("rev_level");
    e->p_dly_fdbk  = find_pot("dly_fdbk");  e->p_dly_tone  = find_pot("dly_tone");
    e->p_dly_hpf   = find_pot("dly_hpf");   e->p_dly_level = find_pot("dly_level");
    e->e_dly_time  = find_enum("dly_time");
    e->p_comp      = find_pot("comp");

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

    e->verb.hpf_hz = e->potv[e->p_rev_hpf];
    e->dly.hpf_hz  = e->potv[e->p_dly_hpf];
    sd606_verb_init(&e->verb, e->sample_rate);
    sd606_dly_init(&e->dly, e->sample_rate);
    e->dly.divi = e->env[e->e_dly_time];
    e->dly.bpm  = 120.0f;
    sd606_dly_retime(&e->dly, e->sample_rate);
    e->dly.dcur = e->dly.time_ms * 0.001f * e->sample_rate;   /* no start-up sweep */
    sd606_fx_sync(e);
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

    /*
     * How loud this hit is, from its velocity.
     *
     * Accent is the TOP: a full-velocity hit reaches it whatever Velocity is
     * set to, and softer hits come down from there. Velocity is how far down
     * -- 0 means every hit plays at Accent, which is what this kit sounded
     * like before velocity existed at all.
     *
     * Ported from 9W9 along with the three wrong turns it took first, each
     * worth not repeating:
     *   1. The machine's accent SWITCH, reproduced literally. From a
     *      sequencer that is a 6 dB cliff between velocity 99 and 100 with a
     *      flat shelf either side, and velocity looks broken. 6W6 shipped
     *      this in v1.2.0.
     *   2. One line, but pivoting mid-range, so turning Velocity UP made hard
     *      hits louder -- the knob moved the kit's loudness, not its dynamics.
     *   3. Anchored at 1.0 with Accent deleted. Tidy, and it quietly drops
     *      the whole kit 6 dB: 1.0 is the UNACCENTED level, and a pattern
     *      from Move (velocity 100 and up) had always played at the accented
     *      one.
     * Anchoring at Accent keeps the reference level where it has always been,
     * and the knob only ever carves downwards from it.
     */
    const int   vi = velocity > 127 ? 127 : velocity;
    const float accent = SD606_FULL_VELOCITY_GAIN
                       * (1.0f - e->potv[e->p_vel_depth]
                                 * (1.0f - (float)vi * (1.0f / 127.0f)));

    e->rt[voice].hit_gain   = accent;
    e->rt[voice].choke_gain = 1.0f;
    e->rt[voice].choke_step = 0.0f;
    e->rt[voice].crush_st[0] = 0.0f;
    e->rt[voice].crush_st[1] = 0.0f;

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
    case SD606_BD:
        /* No drift argument: the 606's kick does not wander from hit to hit,
         * and the control that could make it (bd_drift) is gone. */
        e->bd.trigger(e->potv[e->bd_attack], decay, tune, 0.0f);
        break;
    case SD606_SD:
        /* The 606's snare has no decay control, so neither do we: pinned at
         * the value fitted against the hardware recording (old pot 76 of
         * 127). `decay` is ignored for this voice. */
        e->sd.trigger(SD606_SD_DECAY_FIXED, tune, e->potv[e->sd_snappy],
                      e->potv[e->sd_tone]);
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
    const int dpot = e->pot[s.drive];
    float shaped;
    if(dpot <= 0)
    {
        shaped = raw;                       /* knob fully down: nothing at all */
    }
    else
    {
        shaped = sd606_shape_st(raw, e->potv[s.drive], e->env[s.dist], r.crush_st);
        if(dpot < SD606_DRIVE_WET_POT)      /* fade the stage in over 0..8 */
            shaped = raw + ((float)dpot / (float)SD606_DRIVE_WET_POT) * (shaped - raw);
    }
    return shaped * e->potv[s.level] * kVoiceTrim[v] * r.hit_gain * r.choke_gain;
}

/* A silent lane must not cost a process() call — the metal voices sum 47
 * sines each and three of them can be ringing at once. Guarding on the gate
 * gain also means a muted lane keeps rendering until its fade completes, then
 * disappears. */
#define SD606_LANE(vid, expr)                                                 \
    do { if(e->rt[vid].choke_gain > 0.0f) {                                   \
             const float sv = voice_sample(e, vid, (expr));                   \
             mix   += sv;                                                     \
             /* post-fader sends: what you hear is what you send */           \
             send_r += sv * e->potv[e->slot[vid].rev];                        \
             send_d += sv * e->potv[e->slot[vid].dly];                        \
         } } while(0)

void sd606_render(sd606_engine_t *e, float *out, int frames)
{
    const int   mdist  = e->env[e->e_master_dist];
    const float mdrive = e->potv[e->p_master_drive];
    const int   mpot   = e->pot[e->p_master_drive];
    const float vol    = e->potv[e->p_volume];
    const float comp   = e->potv[e->p_comp];

    for(int i = 0; i < frames; ++i)
    {
        float mix = 0.0f, send_r = 0.0f, send_d = 0.0f;
        SD606_LANE(SD606_BD, e->bd.process());
        SD606_LANE(SD606_SD, e->sd.process());
        SD606_LANE(SD606_LT, e->lt.process());
        SD606_LANE(SD606_HT, e->ht.process());
        SD606_LANE(SD606_CH, e->ch.process());
        SD606_LANE(SD606_OH, e->oh.process());
        SD606_LANE(SD606_CY, e->cy.process());
        SD606_LANE(SD606_CP, e->cp.process());

        /* The wet returns BEFORE the master distortion, so that stage works
         * on the whole picture rather than just the dry kit.
         *
         * With every send at 0 (the default) both ticks are fed exactly 0.0
         * from silent state and return exactly 0.0, so this cannot perturb a
         * single sample -- which is what tools/ab_null.sh checks. They are
         * still ticked, not branched around: a send turned down while the
         * tail is ringing must let that tail finish, and a branch on
         * "input == 0" would chop it off. */
        mix += sd606_verb_tick(&e->verb, send_r);
        mix += sd606_dly_tick(&e->dly, send_d, e->sample_rate);

        /* Master stage. Option 0 is Off, so the kit can be left alone. */
        if(mdist > 0 && mpot > 0)
        {
            float shaped = sd606_shape_st(mix, mdrive, mdist - 1, e->crush_master);
            if(mpot < SD606_DRIVE_WET_POT)
                shaped = mix + ((float)mpot / (float)SD606_DRIVE_WET_POT) * (shaped - mix);
            mix = shaped;
        }
        /* Glue after the distortion, before the volume -- and skipped
         * entirely at zero, which is the default, so it cannot colour a kit
         * nobody asked it to touch. */
        if(comp > 0.001f) mix = sd606_glue_tick(&e->glue, mix, comp, e->sample_rate);
        mix *= vol;

        if(!(mix > -8.0f && mix < 8.0f)) mix = 0.0f;   /* also catches NaN */
        out[i] = mix;
    }
}

/* ---- parameters ------------------------------------------------------- */

/* The FX structs hold engineering values, not pot positions, so a write to
 * any of their keys has to be pushed across. Cheap and rare; never per sample. */
static void sd606_fx_sync(sd606_engine_t *e)
{
    e->verb.decay = e->potv[e->p_rev_decay];
    e->verb.tone  = e->potv[e->p_rev_tone];
    e->verb.level = e->potv[e->p_rev_level];
    if(e->verb.hpf_hz != e->potv[e->p_rev_hpf])
    {
        e->verb.hpf_hz = e->potv[e->p_rev_hpf];
        e->verb.hp.setHighPass(e->verb.hpf_hz, 0.7071f);
    }
    e->dly.fdbk  = e->potv[e->p_dly_fdbk];
    e->dly.tone  = e->potv[e->p_dly_tone];
    e->dly.level = e->potv[e->p_dly_level];
    if(e->dly.hpf_hz != e->potv[e->p_dly_hpf])
    {
        e->dly.hpf_hz = e->potv[e->p_dly_hpf];
        e->dly.hp.setHighPass(e->dly.hpf_hz, 0.7071f);
    }
    if(e->dly.divi != e->env[e->e_dly_time])
    {
        e->dly.divi = e->env[e->e_dly_time];
        sd606_dly_retime(&e->dly, e->sample_rate);
    }
}

int sd606_set_param(sd606_engine_t *e, const char *key, const char *val)
{
    /* Tempo, pushed in by the plugin each block. Not a pot: it has no UI and
     * no stored position, it is just what the host says the BPM is. */
    if(!strcmp(key, "dly_bpm"))
    {
        const float bpm = (float)atof(val);
        if(bpm > 20.0f && bpm != e->dly.bpm)
        {
            e->dly.bpm = bpm;
            sd606_dly_retime(&e->dly, e->sample_rate);
        }
        return 1;
    }
    /* "default" resets a control to its fitted default. The kit's defaults
     * are not centred (they are fitted against a hardware 606), so a UI that
     * wants a reset gesture must not guess 64 — it asks. */
    const int reset = !strcmp(val, "default");
    const int slot = find_pot(key);
    if(slot >= 0)
    {
        int p = reset ? g_sd606_pots[slot].def : (int)(atof(val) + 0.5f);
        if(p < 0) p = 0;
        if(p > 127) p = 127;                 /* clamp, never wrap */
        e->pot[slot]  = p;
        e->potv[slot] = pot_value(slot, p);
        sd606_fx_sync(e);
        return 1;
    }
    const int es = find_enum(key);
    if(es >= 0)
    {
        int v = reset ? g_sd606_enums[es].def : (int)(atof(val) + 0.5f);
        if(v < 0) v = 0;
        if(v >= g_sd606_enums[es].count) v = g_sd606_enums[es].count - 1;
        e->env[es] = v;
        sd606_fx_sync(e);
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

/* ---- v1 -> v2 migration -------------------------------------------------
 *
 * v1 stored drive as a position on a (0.2, 8) EXP pot and the distortion type
 * as an index into a 4-option list (Diode, Clip, Fold, Crush). v2 uses a
 * (0.85, 12) pot and a 7-option list with Fold and Crush moved to the end.
 * Loading a v1 blob without this would quietly retune every drive knob and
 * turn every saved Fold into SAT and every Crush into BFZ.
 *
 * Drive is converted through the ENGINEERING VALUE, not the position: recover
 * what the old pot meant, then re-solve for the position that means the same
 * thing now. Old positions below ~52 were attenuating settings the new range
 * cannot express at all; they clamp to 0, which matches 9W9.
 */
/* The pot table as SHIPPED IN v1.0.0, in its exact order. v2 deletes
 * bd_drift and sd_decay and appends the sends, the FX and Comp, so a v1 blob
 * cannot be read positionally any more -- position 4 used to be bd_drive and
 * is now bd_level. Values are therefore scattered BY NAME, and the two
 * deleted keys simply have nowhere to land. */
static const char *const kV1PotKeys[40] = {
    "bd_tune",
    "bd_decay",
    "bd_attack",
    "bd_drift",
    "bd_drive",
    "bd_level",
    "sd_tune",
    "sd_decay",
    "sd_snappy",
    "sd_tone",
    "sd_drive",
    "sd_level",
    "lt_tune",
    "lt_decay",
    "lt_drive",
    "lt_level",
    "ht_tune",
    "ht_decay",
    "ht_drive",
    "ht_level",
    "ch_tune",
    "ch_decay",
    "ch_drive",
    "ch_level",
    "oh_tune",
    "oh_decay",
    "oh_drive",
    "oh_level",
    "cy_tune",
    "cy_decay",
    "cy_drive",
    "cy_level",
    "cp_tune",
    "cp_decay",
    "cp_noise",
    "cp_drive",
    "cp_level",
    "master_drive",
    "volume",
    "accent",
};

/* The pot table as shipped in v1.2.0. v3 deletes `accent`, which sat between
 * volume and comp, so everything after it moved and a v2 blob cannot be read
 * positionally either. Same treatment as v1: scattered by name. */
static const char *const kV2PotKeys[64] = {
    "bd_tune",
    "bd_decay",
    "bd_attack",
    "bd_drive",
    "bd_level",
    "sd_tune",
    "sd_snappy",
    "sd_tone",
    "sd_drive",
    "sd_level",
    "lt_tune",
    "lt_decay",
    "lt_drive",
    "lt_level",
    "ht_tune",
    "ht_decay",
    "ht_drive",
    "ht_level",
    "ch_tune",
    "ch_decay",
    "ch_drive",
    "ch_level",
    "oh_tune",
    "oh_decay",
    "oh_drive",
    "oh_level",
    "cy_tune",
    "cy_decay",
    "cy_drive",
    "cy_level",
    "cp_tune",
    "cp_decay",
    "cp_noise",
    "cp_drive",
    "cp_level",
    "master_drive",
    "volume",
    "accent",
    "comp",
    "vel_depth",
    "bd_rev",
    "bd_dly",
    "sd_rev",
    "sd_dly",
    "lt_rev",
    "lt_dly",
    "ht_rev",
    "ht_dly",
    "ch_rev",
    "ch_dly",
    "oh_rev",
    "oh_dly",
    "cy_rev",
    "cy_dly",
    "cp_rev",
    "cp_dly",
    "rev_decay",
    "rev_tone",
    "rev_hpf",
    "rev_level",
    "dly_fdbk",
    "dly_tone",
    "dly_hpf",
    "dly_level",
};

static void migrate_v1(sd606_engine_t *e)
{
    for(int i = 0; i < SD606_NUM_POTS; ++i)
    {
        const char *k = g_sd606_pots[i].key;
        const size_t n = strlen(k);
        const int is_drive = (n >= 6 && !strcmp(k + n - 6, "_drive")) ||
                             !strcmp(k, "master_drive");
        if(!is_drive) continue;
        const float old_val = 0.2f * powf(8.0f / 0.2f, (float)e->pot[i] / 127.0f);
        float p = 127.0f * logf(old_val / 0.85f) / logf(12.0f / 0.85f);
        if(p < 0.0f)   p = 0.0f;
        if(p > 127.0f) p = 127.0f;
        e->pot[i]  = (int)(p + 0.5f);
        e->potv[i] = pot_value(i, e->pot[i]);
    }
    /* 0 Diode, 1 Clip stay put; Fold 2->5, Crush 3->6. master_dist carries a
     * leading "Off", so its indices shift by one: 3->6, 4->7. */
    static const int kVoiceRemap[4]  = { 0, 1, 5, 6 };
    static const int kMasterRemap[5] = { 0, 1, 2, 6, 7 };
    for(int i = 0; i < SD606_NUM_ENUMS; ++i)
    {
        const char *k = g_sd606_enums[i].key;
        const size_t n = strlen(k);
        if(n >= 10 && !strcmp(k + n - 10, "_dist_type"))
        {
            if(e->env[i] >= 0 && e->env[i] < 4) e->env[i] = kVoiceRemap[e->env[i]];
        }
        else if(!strcmp(k, "master_dist"))
        {
            if(e->env[i] >= 0 && e->env[i] < 5) e->env[i] = kMasterRemap[e->env[i]];
        }
    }
}

void sd606_deserialize(sd606_engine_t *e, const char *json)
{
    if(!json || !*json) return;

    /* The version was written from the start but never read. It is read now,
     * because v2 needs it. A blob with no "v" at all predates nothing we
     * shipped, but treat it as v1: that is the conservative reading. */
    int version = 1;
    {
        const char *vp = strstr(json, "\"v\"");
        if(vp) { vp = strchr(vp, ':'); if(vp) version = (int)strtol(vp + 1, NULL, 10); }
    }
    const char *p = strstr(json, "\"pots\"");
    int vals[SD606_NUM_POTS > SD606_NUM_ENUMS ? SD606_NUM_POTS : SD606_NUM_ENUMS];
    int got = 0;

    /* Older blobs carry an older table, so they are placed by NAME against
     * the order that shipped with them; v3+ blobs are positional against the
     * current table. Keys that no longer exist simply have nowhere to land. */
    if(version < 3)
    {
        const char *const *keys = version < 2 ? kV1PotKeys : kV2PotKeys;
        const int n = version < 2 ? 40 : 64;
        int old[64];
        p = scan_ints(p, old, n, &got);
        for(int i = 0; i < got && i < n; ++i)
        {
            const int slot = find_pot(keys[i]);
            if(slot < 0) continue;      /* bd_drift, sd_decay, accent: dropped */
            int v = old[i] < 0 ? 0 : (old[i] > 127 ? 127 : old[i]);
            e->pot[slot]  = v;
            e->potv[slot] = pot_value(slot, v);
        }
    }
    else
    {
        p = scan_ints(p, vals, SD606_NUM_POTS, &got);
        for(int i = 0; i < got; ++i)
        {
            int v = vals[i] < 0 ? 0 : (vals[i] > 127 ? 127 : vals[i]);
            e->pot[i]  = v;
            e->potv[i] = pot_value(i, v);
        }
    }

    const char *q = strstr(json, "\"enums\"");
    scan_ints(q, vals, SD606_NUM_ENUMS, &got);
    for(int i = 0; i < got; ++i)
    {
        int v = vals[i] < 0 ? 0 : vals[i];
        if(v >= g_sd606_enums[i].count) v = g_sd606_enums[i].count - 1;
        e->env[i] = v;
    }

    if(version < 2) migrate_v1(e);   /* drive range + distortion remap */
    sd606_fx_sync(e);

    const char *mp = strstr(json, "\"mutes\"");
    if(mp) { mp = strchr(mp, ':'); if(mp) e->mutes = (unsigned)strtoul(mp + 1, NULL, 10)
                                                     & ((1u << SD606_NUM_VOICES) - 1u); }
}

/* Read-only view of the kit balance, for the trim-measurement probe in
 * tools/. Not part of the plugin surface. */
extern "C" const float *sd606_debug_trim(void) { return kVoiceTrim; }

/* Report every slot index the engine resolved, so a probe can assert none is
 * -1. find_pot returning -1 means potv[-1] on the audio path. */
extern "C" int sd606_debug_slots(sd606_engine_t *e, const char **names, int *idx, int max)
{
    int n = 0;
    #define SLOT(nm, v) do { if(n < max) { names[n] = nm; idx[n] = (v); ++n; } } while(0)
    for(int v = 0; v < SD606_NUM_VOICES; ++v)
    {
        SLOT("tune", e->slot[v].tune);   SLOT("decay", e->slot[v].decay);
        SLOT("drive", e->slot[v].drive); SLOT("level", e->slot[v].level);
        SLOT("dist", e->slot[v].dist);   SLOT("rev", e->slot[v].rev);
        SLOT("dly", e->slot[v].dly);
    }
    SLOT("bd_attack", e->bd_attack); SLOT("sd_snappy", e->sd_snappy);
    SLOT("sd_tone", e->sd_tone);     SLOT("cp_noise", e->cp_noise);
    SLOT("master_drive", e->p_master_drive); SLOT("volume", e->p_volume);
    SLOT("vel_depth", e->p_vel_depth); SLOT("comp", e->p_comp);
    SLOT("rev_decay", e->p_rev_decay); SLOT("rev_tone", e->p_rev_tone);
    SLOT("rev_hpf", e->p_rev_hpf);     SLOT("rev_level", e->p_rev_level);
    SLOT("dly_fdbk", e->p_dly_fdbk);   SLOT("dly_tone", e->p_dly_tone);
    SLOT("dly_hpf", e->p_dly_hpf);     SLOT("dly_level", e->p_dly_level);
    SLOT("master_dist", e->e_master_dist); SLOT("choke", e->e_choke);
    SLOT("note_map", e->e_note_map);       SLOT("dly_time", e->e_dly_time);
    #undef SLOT
    return n;
}
