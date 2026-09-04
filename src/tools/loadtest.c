/* End-to-end test of the REAL dsp.so exactly as the chain host uses it:
 * dlopen -> move_plugin_init_v2 -> create_instance -> params -> midi -> render.
 *
 * Built for aarch64 and run ON THE MOVE. A loadtest that is handed the .so
 * directly proves the plugin is correct; this one additionally checks the
 * things the HOST consumes (chain_params present, ui_hierarchy served EMPTY so
 * ui_chain.js engages) and asserts on AUDIBLE behaviour — that a parameter
 * actually changes the sound — rather than on "nothing crashed".
 */
#include <dlfcn.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plugin_api_v1.h"

static void hlog(const char *m) { printf("   [host] %s\n", m); }

/* Fake transport for the sequencer test: advances one beat per ~11 blocks. */
static double g_beats = -1.0;
static double fake_beat_position(void) { return g_beats; }
static int    fake_clock_status(void)  { return g_beats >= 0.0 ? 2 : 1; }

static int fails = 0;
#define CHECK(c, msg) do { if(!(c)) { printf("FAIL: %s\n", msg); fails++; } \
                           else printf("ok  : %s\n", msg); } while(0)

static plugin_api_v2_t *api;
static void *inst;
static int16_t block[128 * 2];

/* Render `blocks` blocks and return peak absolute sample, 0..1. */
static double render_peak(int blocks)
{
    int peak = 0;
    for(int b = 0; b < blocks; ++b)
    {
        api->render_block(inst, block, 128);
        for(int i = 0; i < 128 * 2; ++i)
        {
            const int a = block[i] < 0 ? -block[i] : block[i];
            if(a > peak) peak = a;
        }
    }
    return (double)peak / 32767.0;
}

/* Peak is too blunt to tell distortion types apart -- Diode and Fold land
 * within 0.0008 of each other on a kick. Hash the rendered samples instead. */
static unsigned render_hash(int blocks)
{
    unsigned h = 2166136261u;
    for(int b = 0; b < blocks; ++b)
    {
        api->render_block(inst, block, 128);
        for(int i = 0; i < 128 * 2; ++i) h = (h ^ (unsigned)(unsigned short)block[i]) * 16777619u;
    }
    return h;
}

static void note_on(int note, int vel)
{
    const uint8_t msg[3] = { 0x90, (uint8_t)note, (uint8_t)vel };
    api->on_midi(inst, msg, 3, 0);
}

int main(int argc, char **argv)
{
    const char *so  = argc > 1 ? argv[1] : "./dsp.so";
    const char *dir = argc > 2 ? argv[2] : ".";
    char buf[65536];

    void *h = dlopen(so, RTLD_NOW);
    if(!h) { printf("FAIL dlopen: %s\n", dlerror()); return 1; }
    CHECK(1, "dlopen dsp.so");

    move_plugin_init_v2_fn init = (move_plugin_init_v2_fn)dlsym(h, "move_plugin_init_v2");
    CHECK(init != NULL, "move_plugin_init_v2 symbol present");
    if(!init) return 1;

    static host_api_v1_t host;
    memset(&host, 0, sizeof(host));
    host.api_version = 1; host.sample_rate = 44100; host.frames_per_block = 128;
    host.log = hlog;
    host.get_beat_position = fake_beat_position;
    host.get_clock_status  = fake_clock_status;

    api = init(&host);
    CHECK(api && api->api_version == 2, "api_version == 2");
    CHECK(api->create_instance && api->render_block && api->on_midi &&
          api->set_param && api->get_param, "all v2 entry points non-NULL");

    inst = api->create_instance(dir, NULL);
    CHECK(inst != NULL, "create_instance");
    if(!inst) return 1;

    /* ---- the payloads the Shadow UI actually reads ---- */
    int n = api->get_param(inst, "chain_params", buf, sizeof(buf));
    CHECK(n > 100 && buf[0] == '[', "get_param(chain_params) returns a JSON array");
    printf("      chain_params = %d bytes\n", n);
    CHECK(strstr(buf, "\"hh_choke\"") != NULL, "chain_params advertises hh_choke");
    CHECK(strstr(buf, "\"master_dist\"") != NULL, "chain_params advertises master_dist");
    CHECK(strstr(buf, "\"Crush\"") != NULL && strstr(buf, "\"PDIST\"") != NULL,
          "master distortion offers all seven types (Crush, PDIST present)");

    /* EMPTY, not an error. The host's load gate treats an error as "the read
     * did not complete" and waits forever, which is what hung Swap Module into
     * 6W6 on a Loading... card. "" means served-and-empty: fall back to
     * ui_chain.js at once. */
    {
        char h[8];
        memset(h, 0x7F, sizeof h);
        int m = api->get_param(inst, "ui_hierarchy", h, sizeof(h));
        CHECK(m == 0 && h[0] == 0,
              "ui_hierarchy served EMPTY, not as an error (so the load gate does not hang)");
    }
    n = api->get_param(inst, "ui_pages", buf, sizeof(buf));
    CHECK(n > 100 && strstr(buf, "\"levels\"") != NULL,
          "ui_pages serves the hierarchy for the param_pages binding");
    CHECK(strstr(buf, "\"cy\"") != NULL, "cymbal has a page");

    /* ---- pots are 0..127 positions, like the hardware panel ---- */
    api->set_param(inst, "bd_tune", "77");
    api->get_param(inst, "bd_tune", buf, sizeof(buf));
    CHECK(atoi(buf) == 77, "pot bd_tune round-trips (77)");
    api->set_param(inst, "oh_decay", "999");
    api->get_param(inst, "oh_decay", buf, sizeof(buf));
    CHECK(atoi(buf) == 127, "pot oh_decay clamps 999 -> 127");
    api->set_param(inst, "oh_decay", "-5");
    api->get_param(inst, "oh_decay", buf, sizeof(buf));
    CHECK(atoi(buf) == 0, "pot oh_decay clamps -5 -> 0");
    api->set_param(inst, "oh_decay", "102");   /* clamp tests must not leak into
                                                  the behavioural ones below */
    api->set_param(inst, "bd_tune", "64");
    api->set_param(inst, "hh_choke", "9");
    api->get_param(inst, "hh_choke", buf, sizeof(buf));
    CHECK(atoi(buf) == 2, "enum hh_choke clamps to its last option");
    CHECK(api->get_param(inst, "no_such_key", buf, sizeof(buf)) < 0,
          "unknown key reports -1 rather than lying");
    api->set_param(inst, "bd_decay", "3");
    api->set_param(inst, "bd_decay", "default");
    api->get_param(inst, "bd_decay", buf, sizeof(buf));
    CHECK(atoi(buf) == 24, "set_param(key, \"default\") restores the fitted default (bd_decay 24)");
    api->set_param(inst, "hh_choke", "default");
    api->get_param(inst, "hh_choke", buf, sizeof(buf));
    CHECK(atoi(buf) == 1, "\"default\" works for enums too (hh_choke CH > OH)");
    api->set_param(inst, "ui_focus", "6");
    api->get_param(inst, "ui_focus", buf, sizeof(buf));
    CHECK(!strcmp(buf, "6"), "ui_focus round-trips (the editor publishes, the panel follows)");
    note_on(69, 90);                                     /* a hand-played snare */
    api->get_param(inst, "ui_focus", buf, sizeof(buf));
    CHECK(!strcmp(buf, "1"), "a hand-played pad moves ui_focus on its own (lane 1, no editor needed)");
    render_peak(400);
    api->get_param(inst, "chain_params", buf, sizeof(buf));
    CHECK(strstr(buf, "\"key\":\"ui_focus\"") && strstr(buf, "\"key\":\"mutes\""),
          "chain_params advertises ui_focus and mutes (so the remote manager seeds them)");
    api->get_param(inst, "chain_params", buf, sizeof(buf));
    /* the picker name is voice-qualified ("BD Decay"); the bare "Decay" is
     * the page label and lives in the hierarchy, not here */
    CHECK(strstr(buf, "\"key\":\"bd_decay\",\"name\":\"BD Decay\",\"type\":\"int\",\"min\":0,\"max\":127,\"default\":24") != NULL,
          "chain_params advertises the fitted default, under its picker name");
    CHECK(strstr(buf, "\"name\":\"CH Decay\"") && strstr(buf, "\"name\":\"REV Decay\""),
          "every voice and bus qualifies its params for the LFO picker");

    /* ---- silence when nothing is played ---- */
    CHECK(render_peak(40) < 0.0005, "silent with no triggers");

    /* ---- every pad sounds, on the raw pad map and the drum rack map ---- */
    const int pads[8]  = { 68, 69, 70, 71, 76, 77, 78, 79 };
    const int rack[8]  = { 36, 37, 38, 39, 40, 41, 42, 43 };
    const char *ids[8] = { "bd", "sd", "lt", "ht", "ch", "oh", "cy", "cp" };
    for(int v = 0; v < 8; ++v)
    {
        api->set_param(inst, "state", "");             /* no-op, keeps defaults */
        note_on(pads[v], 90);
        const double p = render_peak(60);
        char msg[96]; snprintf(msg, sizeof(msg), "pad note %d sounds (%s), peak %.3f",
                               pads[v], ids[v], p);
        CHECK(p > 0.02, msg);
        render_peak(400);                               /* let the tail die */
    }
    for(int v = 0; v < 8; ++v)
    {
        note_on(rack[v], 90);
        const double p = render_peak(60);
        char msg[96]; snprintf(msg, sizeof(msg), "drum-rack note %d sounds (%s)",
                               rack[v], ids[v]);
        CHECK(p > 0.02, msg);
        render_peak(400);
    }

    /* ---- General MIDI map is a real alternative, not a stub ---- */
    api->set_param(inst, "note_map", "1");
    note_on(42, 90);                                     /* GM closed hat */
    CHECK(render_peak(60) > 0.02, "GM note 42 sounds (closed hat)");
    render_peak(400);
    api->set_param(inst, "note_map", "0");

    /*
     * ---- velocity ----
     * Accent is the TOP of the range, not a switch partway up it. The checks
     * assert the PROPERTIES that matter rather than any particular curve:
     * monotonic, no step where the old threshold used to be, genuinely flat
     * at depth 0, and never boosting a full-velocity hit.
     */
    {
        api->set_param(inst, "mutes", "0");
        api->set_param(inst, "vel_depth", "127");
        const int vels[6] = { 20, 64, 90, 99, 100, 127 };
        double lv[6];
        for(int i = 0; i < 6; ++i)
        {
            render_peak(600); note_on(68, vels[i]); lv[i] = render_peak(120);
        }
        int mono = 1;
        for(int i = 1; i < 6; ++i) if(lv[i] < lv[i-1] * 0.98) mono = 0;
        char m2[180];
        snprintf(m2, sizeof m2, "velocity is monotonic (%.3f %.3f %.3f %.3f %.3f %.3f)",
                 lv[0], lv[1], lv[2], lv[3], lv[4], lv[5]);
        CHECK(mono, m2);
        snprintf(m2, sizeof m2, "no step where the old accent threshold was (99 %.3f -> 100 %.3f)",
                 lv[3], lv[4]);
        CHECK(fabs(lv[4] - lv[3]) < lv[3] * 0.05, m2);
        snprintf(m2, sizeof m2, "and the range is wide open at full depth (%.1f dB)",
                 20.0 * log10(lv[5] / lv[0]));
        CHECK(lv[5] > lv[0] * 4.0, m2);

        /* depth 0: velocity does nothing, accented velocities included */
        api->set_param(inst, "vel_depth", "0");
        double f0[4]; const int fv[4] = { 20, 90, 110, 127 };
        for(int i = 0; i < 4; ++i)
        {
            render_peak(600); note_on(68, fv[i]); f0[i] = render_peak(120);
        }
        double lo = f0[0], hi = f0[0];
        for(int i = 1; i < 4; ++i) { lo = fmin(lo, f0[i]); hi = fmax(hi, f0[i]); }
        snprintf(m2, sizeof m2, "Velocity 0 means velocity 0 (spread %.2f dB over 20..127)",
                 20.0 * log10(hi / lo));
        CHECK(hi < lo * 1.02, m2);

        /* the knob only ever carves DOWN: a full hit is the same at any depth */
        snprintf(m2, sizeof m2, "a full-velocity hit never gets louder with depth (%.3f vs %.3f)",
                 lv[5], f0[3]);
        CHECK(fabs(lv[5] - f0[3]) < f0[3] * 0.02, m2);

        api->set_param(inst, "vel_depth", "127");
        render_peak(600);
    }

    /* ---- mutes silence a lane and let it back in ---- */
    api->set_param(inst, "mutes", "1");                  /* bit 0 = bass drum */
    note_on(68, 110);
    CHECK(render_peak(60) < 0.005, "muted lane swallows its trigger");
    api->set_param(inst, "mutes", "0");
    note_on(68, 110);
    CHECK(render_peak(60) > 0.02, "unmuted lane sounds again");
    render_peak(600);

    /*
     * ---- hi-hat choke ----
     * The point of the switch. Ring an open hat, wait, then either hit a
     * closed hat or don't, and compare what is left of the open tail. This
     * asserts the AUDIBLE effect; a param round-trip would have proved nothing.
     */
    {
        /* Velocity does NOT scale level on this kit — like the hardware, it
         * only picks accent — so the closed hat cannot be made quiet. Separate
         * the two in TIME instead: shorten CH to its minimum, let it die, then
         * measure what is left of the open hat's tail. */
        api->set_param(inst, "hh_choke", "0");           /* Off */
        api->set_param(inst, "mutes", "0");
        api->set_param(inst, "ch_decay", "0");

        note_on(77, 100);                                /* open hat */
        render_peak(30);                                 /* let it establish */
        render_peak(40);                                 /* the CH window */
        const double free_tail = render_peak(60);

        render_peak(900);                                /* silence between runs */

        api->set_param(inst, "hh_choke", "1");           /* CH > OH */
        note_on(77, 100);
        render_peak(30);
        note_on(76, 90);                                 /* closed hat */
        render_peak(40);                                 /* CH is gone by here */
        const double choked_tail = render_peak(60);

        char msg[112];
        snprintf(msg, sizeof(msg), "CH chokes OH (tail %.4f -> %.4f)",
                 free_tail, choked_tail);
        CHECK(choked_tail < free_tail * 0.25, msg);

        render_peak(900);

        api->set_param(inst, "hh_choke", "0");
        note_on(77, 100);
        render_peak(30);
        note_on(76, 90);
        render_peak(40);
        const double unchoked = render_peak(60);
        snprintf(msg, sizeof(msg),
                 "choke Off leaves the tail alone (%.4f vs %.4f)",
                 unchoked, free_tail);
        CHECK(unchoked > free_tail * 0.5, msg);

        api->set_param(inst, "hh_choke", "1");
        api->set_param(inst, "ch_decay", "102");
    }
    render_peak(900);

    /* ---- a pot change must change the SOUND, not just the readback ---- */
    {
        api->set_param(inst, "bd_decay", "127");
        note_on(68, 90);
        render_peak(120);
        const double long_tail = render_peak(60);
        render_peak(900);

        api->set_param(inst, "bd_decay", "4");
        note_on(68, 90);
        render_peak(120);
        const double short_tail = render_peak(60);
        char msg[112];
        snprintf(msg, sizeof(msg), "bd_decay changes the audio (%.4f -> %.4f)",
                 long_tail, short_tail);
        CHECK(short_tail < long_tail * 0.5, msg);
        api->set_param(inst, "bd_decay", "24");
    }
    render_peak(900);

    /* ---- master distortion is wired ---- */
    {
        api->set_param(inst, "master_dist", "0");        /* Off */
        note_on(68, 90);
        const double clean = render_peak(60);
        render_peak(900);
        api->set_param(inst, "master_dist", "2");        /* Hard Clip */
        api->set_param(inst, "master_drive", "120");
        note_on(68, 90);
        const double dirty = render_peak(60);
        char msg[112];
        snprintf(msg, sizeof(msg), "master distortion alters output (%.3f -> %.3f)",
                 clean, dirty);
        CHECK(fabs(dirty - clean) > 0.02, msg);
        api->set_param(inst, "master_dist", "0");
        api->set_param(inst, "master_drive", "55");
    }
    render_peak(900);

    /* ---- state round-trip, including the sequencer tail ---- */
    api->set_param(inst, "sd_snappy", "17");
    api->set_param(inst, "cy_level", "111");
    api->set_param(inst, "hh_choke", "2");
    api->set_param(inst, "mutes", "5");
    n = api->get_param(inst, "state", buf, sizeof(buf));
    CHECK(n > 20, "get_param(state) produces a blob");
    {
        /* static, and the same size as buf: a state blob that outgrew the copy
         * would silently truncate and the round-trip below would "pass" on a
         * corrupted string. */
        static char saved[sizeof(buf)];
        CHECK((size_t)n < sizeof(saved), "state blob fits the round-trip buffer");
        snprintf(saved, sizeof(saved), "%s", buf);

        api->set_param(inst, "sd_snappy", "0");
        api->set_param(inst, "cy_level", "0");
        api->set_param(inst, "hh_choke", "0");
        api->set_param(inst, "mutes", "0");

        api->set_param(inst, "state", saved);
        api->get_param(inst, "sd_snappy", buf, sizeof(buf));
        CHECK(atoi(buf) == 17, "state restores sd_snappy");
        api->get_param(inst, "cy_level", buf, sizeof(buf));
        CHECK(atoi(buf) == 111, "state restores cy_level");
        api->get_param(inst, "hh_choke", buf, sizeof(buf));
        CHECK(atoi(buf) == 2, "state restores hh_choke");
        api->get_param(inst, "mutes", buf, sizeof(buf));
        CHECK(atoi(buf) == 5, "state restores mutes");
    }
    /*
     * ---- v1 -> v2 state migration ----
     * v1 stored 40 pots in ITS order; v2 deleted two of them (bd_drift,
     * sd_decay) and appended 25 more, so position 4 used to be bd_drive and is
     * now bd_level. (sd_decay has since come back, appended at the END of the
     * table -- which is why a v1 blob's decay lands again while its position
     * does not.) A v1 blob therefore has to be placed BY NAME. This builds
     * a real v1 blob rather than relabelling a v2 one -- relabelling would
     * only prove the code agrees with itself.
     *
     * v1 order: bd_tune bd_decay bd_attack bd_drift bd_drive bd_level
     *           sd_tune sd_decay sd_snappy sd_tone sd_drive sd_level ...
     *           ... master_drive volume accent
     * v1 enums: bd_dist sd_dist lt_dist ht_dist ch_dist hh_choke
     *           oh_dist cy_dist cp_dist master_dist note_map
     */
    /*
     * ---- a v3 blob written BEFORE sd_decay was appended ----
     * The table only ever grows, so an older v3 blob is simply short. Every
     * pot it carries must land positionally and the appended one must keep its
     * default -- that is what makes every patch made under v1.3.0 play the
     * snare exactly as it did.
     */
    {
        /* A FRESH instance, which is what the host does when it loads a patch.
         * Deserialize deliberately does not reset the pots it was not given --
         * a short blob must not clobber the rest of the table -- so the
         * default has to come from creation, exactly as it does on the Move. */
        void *fresh = api->create_instance(dir, NULL);
        CHECK(fresh != NULL, "second instance for the short-blob test");
        api->set_param(fresh, "state",
            "{\"v\":3,\"pots\":[40,24,120,8,64,70,64,52,8,64],"
            "\"enums\":[0],\"mutes\":0}");
        api->get_param(fresh, "sd_decay", buf, sizeof(buf));
        CHECK(atoi(buf) == 76,
              "short v3 blob: appended sd_decay keeps its fitted default (76)");
        api->get_param(fresh, "bd_tune", buf, sizeof(buf));
        CHECK(atoi(buf) == 40, "short v3 blob: the pots it DOES carry still land positionally");
        api->destroy_instance(fresh);
    }

    {
        static const char *kV1 =
            "{\"v\":1,\"pots\":["
            "11,22,33,44,55,66,"            /* bd: tune decay attack DRIFT drive level */
            "77,88,99,12,55,64,"            /* sd: tune DECAY snappy tone drive level  */
            "64,64,55,64, 64,64,55,64,"     /* lt, ht */
            "64,64,55,64, 64,64,55,64,"     /* ch, oh */
            "64,64,55,64, 64,64,64,55,64,"  /* cy, cp(+noise) */
            "55,76,42],"                    /* master_drive volume accent */
            "\"enums\":[2,3,0,0,0,1,0,0,0,3,0],\"mutes\":0}";
        api->set_param(inst, "state", kV1);

        api->get_param(inst, "bd_tune", buf, sizeof(buf));
        CHECK(atoi(buf) == 11, "v1 blob: bd_tune lands by name (11)");
        api->get_param(inst, "bd_attack", buf, sizeof(buf));
        CHECK(atoi(buf) == 33, "v1 blob: bd_attack lands by name (33), not shifted by the deleted Drift");
        api->get_param(inst, "bd_level", buf, sizeof(buf));
        CHECK(atoi(buf) == 66, "v1 blob: bd_level lands by name (66)");
        api->get_param(inst, "sd_snappy", buf, sizeof(buf));
        CHECK(atoi(buf) == 99, "v1 blob: sd_snappy lands by name (99), past the deleted sd_decay");
        api->get_param(inst, "sd_tone", buf, sizeof(buf));
        CHECK(atoi(buf) == 12, "v1 blob: sd_tone lands by name (12)");
        CHECK(api->get_param(inst, "bd_drift", buf, sizeof(buf)) < 0,
              "bd_drift no longer exists at all (the 606 kick does not wander)");
        api->get_param(inst, "sd_decay", buf, sizeof(buf));
        CHECK(atoi(buf) == 88,
              "v1 blob: sd_decay restores (88) -- the control is back, appended");

        /* drive converted through the engineering value, not the position */
        api->get_param(inst, "bd_drive", buf, sizeof(buf));
        CHECK(atoi(buf) == 7, "v1 drive pot 55 (unity) migrates to pot 7 — same drive, new range");
        api->get_param(inst, "master_drive", buf, sizeof(buf));
        CHECK(atoi(buf) == 7, "master_drive migrates the same way");
        /* enums remapped: Fold 2->5, Crush 3->6, master Fold 3->6 */
        api->get_param(inst, "bd_dist_type", buf, sizeof(buf));
        CHECK(atoi(buf) == 5, "v1 Fold (2) migrates to the new Fold (5)");
        api->get_param(inst, "sd_dist_type", buf, sizeof(buf));
        CHECK(atoi(buf) == 6, "v1 Crush (3) migrates to the new Crush (6)");
        api->get_param(inst, "master_dist", buf, sizeof(buf));
        CHECK(atoi(buf) == 6, "v1 master Fold (3) migrates to 6");
        /* things v1 never had come up at their defaults, not at garbage */
        api->get_param(inst, "bd_rev", buf, sizeof(buf));
        CHECK(atoi(buf) == 0, "a v1 patch gets the new sends at zero");
        api->get_param(inst, "comp", buf, sizeof(buf));
        CHECK(atoi(buf) == 0, "a v1 patch gets Comp at zero");

        /* A v2 blob (v1.1.x / v1.2.0) is also placed by NAME now, because
         * deleting `accent` renumbered everything after it. Build one the
         * same way: 64 pots in the v1.2.0 order. */
        {
            static const char *kV2 =
                "{\"v\":2,\"pots\":["
                "11,22,33,8,64,"                    /* bd tune decay attack drive level */
                "70,64,52,8,64,"                    /* sd tune snappy tone drive level */
                "64,124,8,64, 64,124,8,64,"         /* lt, ht */
                "64,102,8,64, 64,102,8,64,"         /* ch, oh */
                "64,102,8,64, 64,102,64,8,64,"      /* cy, cp(+noise) */
                "8,76,99,55,127,"                   /* master_drive volume ACCENT comp vel_depth */
                "0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,"  /* the 16 sends */
                "73,57,62,85,52,51,62,85],"         /* rev, dly */
                "\"enums\":[0,0,0,0,0,1,0,0,0,0,0,7],\"mutes\":0}";
            api->set_param(inst, "state", kV2);
            api->get_param(inst, "bd_attack", buf, sizeof(buf));
            CHECK(atoi(buf) == 33, "v2 blob: bd_attack lands by name (33)");
            api->get_param(inst, "comp", buf, sizeof(buf));
            CHECK(atoi(buf) == 55, "v2 blob: comp lands by name (55), past the deleted Accent");
            api->get_param(inst, "vel_depth", buf, sizeof(buf));
            CHECK(atoi(buf) == 127, "v2 blob: vel_depth lands by name (127)");
            api->get_param(inst, "rev_decay", buf, sizeof(buf));
            CHECK(atoi(buf) == 73, "v2 blob: the FX tail is not shifted either");
            CHECK(api->get_param(inst, "accent", buf, sizeof(buf)) < 0,
                  "accent no longer exists — velocity is the whole range now");
            api->set_param(inst, "state", "");
            api->set_param(inst, "comp", "0");
        }

        /* a v3 blob round-trips positionally, untouched */
        api->set_param(inst, "bd_drive", "40");
        api->set_param(inst, "cy_rev", "99");
        {
            static char v2[8192];
            api->get_param(inst, "state", v2, sizeof(v2));
            CHECK(strstr(v2, "\"v\":3") != NULL, "fresh blobs are written as v3");
            api->set_param(inst, "bd_drive", "0");
            api->set_param(inst, "cy_rev", "0");
            api->set_param(inst, "state", v2);
            api->get_param(inst, "bd_drive", buf, sizeof(buf));
            CHECK(atoi(buf) == 40, "a v3 blob is NOT migrated (drive stays 40)");
            api->get_param(inst, "cy_rev", buf, sizeof(buf));
            CHECK(atoi(buf) == 99, "a v3 blob restores the send pots too");
        }
        api->set_param(inst, "state", "");
        for(int v = 0; v < 8; ++v)
        {
            char k[24];
            snprintf(k, sizeof k, "%s_rev", ids[v]); api->set_param(inst, k, "0");
            snprintf(k, sizeof k, "%s_dly", ids[v]); api->set_param(inst, k, "0");
            snprintf(k, sizeof k, "%s_drive", ids[v]); api->set_param(inst, k, "8");
            snprintf(k, sizeof k, "%s_dist_type", ids[v]); api->set_param(inst, k, "0");
        }
        api->set_param(inst, "master_dist", "0");
        api->set_param(inst, "master_drive", "8");
        api->set_param(inst, "comp", "0");
        api->set_param(inst, "mutes", "0");
    }

    /* ---- the master compressor ----
     * These voices carry FREE-RUNNING noise, so the same note rendered later
     * is never bit-identical to itself -- an exact-equality bypass test
     * inside one instance is impossible, and the real proof that Comp at zero
     * costs nothing is tools/ab_null.sh, which nulls two whole builds. Here,
     * compare LEVELS: two comp=0 runs must agree far more closely with each
     * other than either does with comp at full. */
    {
        api->set_param(inst, "mutes", "0");
        double lvl[3];
        const char *amt[3] = { "0", "0", "127" };
        for(int k = 0; k < 3; ++k)
        {
            api->set_param(inst, "comp", amt[k]);
            render_peak(600);
            note_on(68, 120);
            lvl[k] = render_peak(200);
        }
        const double noise_floor = fabs(lvl[0] - lvl[1]);
        const double effect      = fabs(lvl[0] - lvl[2]);
        char msg2[160];
        snprintf(msg2, sizeof msg2,
                 "Comp at full moves the bus well past run-to-run noise (%.4f vs %.4f)",
                 effect, noise_floor);
        CHECK(effect > noise_floor * 5.0 && effect > 0.005, msg2);
        api->set_param(inst, "comp", "0");
        render_peak(600);
    }

    /* the seven distortion types must all be reachable and audibly distinct */
    {
        /* the state round-trip above left mutes at 5 -- bit 0 is the bass drum,
         * and a muted lane renders silence, which is identical for every
         * distortion type. Clear it, or this measures nothing. */
        api->set_param(inst, "mutes", "0");
        api->set_param(inst, "bd_drive", "100");
        unsigned sig[7]; int distinct = 1;
        for(int t = 0; t < 7; ++t)
        {
            char v[8]; snprintf(v, sizeof v, "%d", t);
            render_peak(600);                      /* let the tail die */
            api->set_param(inst, "bd_dist_type", v);
            note_on(68, 90);
            sig[t] = render_hash(200);
        }
        for(int a = 0; a < 7 && distinct; ++a)
            for(int b2 = a + 1; b2 < 7; ++b2)
                if(sig[a] == sig[b2]) { distinct = 0; break; }
        CHECK(distinct, "all 7 distortion types produce pairwise-different output");
        api->set_param(inst, "bd_dist_type", "0");
        api->set_param(inst, "bd_drive", "8");
        render_peak(600);
    }

    /* A blob shorter than the current pot table is a patch saved before a
     * control was added. The contract is that the missing tail is left as it
     * is — not reset, and above all not read off the end of the array. */
    api->set_param(inst, "cy_level", "111");
    api->set_param(inst, "state", "{\"v\":1,\"pots\":[64,64],\"enums\":[0]}");
    api->get_param(inst, "cy_level", buf, sizeof(buf));
    CHECK(atoi(buf) == 111, "short state blob leaves the missing tail untouched");
    api->get_param(inst, "bd_tune", buf, sizeof(buf));
    CHECK(atoi(buf) == 64, "short state blob does apply the entries it carries");

    /*
     * ---- there is no internal sequencer, and a running transport proves it ----
     *
     * The module used to own 8 lanes x 16 steps clocked off get_beat_position().
     * It is gone: Move's own sequencer drives 6W6 over MIDI, one lane per drum.
     * The guard is the interesting direction -- START the transport and require
     * SILENCE. A leftover trigger loop would sound here and nowhere else, since
     * every other test in this file renders with the transport stopped.
     */
    api->set_param(inst, "mutes", "0");
    {
        char k[16];
        for(int v = 0; v < 8; ++v)
        {
            snprintf(k, sizeof(k), "seq_%s", ids[v]);
            api->set_param(inst, k, "1");        /* ignored: no such key */
            char m[40]; snprintf(m, sizeof m, "%s is gone", k);
            CHECK(api->get_param(inst, k, buf, sizeof(buf)) < 0, m);
        }
        CHECK(api->get_param(inst, "seq_voice", buf, sizeof(buf)) < 0,
              "seq_voice is gone");
        CHECK(api->get_param(inst, "seq_pos", buf, sizeof(buf)) < 0,
              "seq_pos is gone");
    }
    g_beats = 0.0;                                /* transport RUNNING */
    render_peak(40);
    CHECK(render_peak(200) < 0.0005,
          "transport running triggers nothing -- no internal sequencer");
    /* A state blob from <=1.5.0 still carries its seq_ tail; it must load and
     * still not make a sound. */
    api->set_param(inst, "state",
        "{\"v\":3,\"pots\":[40,24,120],\"enums\":[0],\"mutes\":0}"
        "seq_bd=65535;seq_sd=65535;");
    CHECK(render_peak(200) < 0.0005,
          "a pre-1.6 blob's seq_ tail is ignored, not replayed");
    g_beats = -1.0;
    render_peak(900);

    api->destroy_instance(inst);
    CHECK(1, "destroy_instance");
    dlclose(h);

    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASS",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
