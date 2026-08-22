/* End-to-end test of the REAL dsp.so exactly as the chain host uses it:
 * dlopen -> move_plugin_init_v2 -> create_instance -> params -> midi -> render.
 *
 * Built for aarch64 and run ON THE MOVE. A loadtest that is handed the .so
 * directly proves the plugin is correct; this one additionally checks the
 * things the HOST consumes (chain_params present, ui_hierarchy absent so
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
    CHECK(strstr(buf, "Bitcrush") != NULL, "master distortion offers Bitcrush");

    CHECK(api->get_param(inst, "ui_hierarchy", buf, sizeof(buf)) < 0,
          "ui_hierarchy absent (so ui_chain.js engages)");
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
    api->get_param(inst, "chain_params", buf, sizeof(buf));
    CHECK(strstr(buf, "\"key\":\"bd_decay\",\"name\":\"Decay\",\"type\":\"int\",\"min\":0,\"max\":127,\"default\":24") != NULL,
          "chain_params advertises the fitted default for bd_decay");

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

    /* ---- accent: velocity >= 100 is genuinely louder ---- */
    render_peak(600);
    note_on(68, 90);
    const double quiet = render_peak(60);
    render_peak(600);
    note_on(68, 120);
    const double loud = render_peak(60);
    {
        char msg[96]; snprintf(msg, sizeof(msg),
            "accent raises level (%.3f -> %.3f)", quiet, loud);
        CHECK(loud > quiet * 1.5, msg);
    }
    render_peak(600);

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
    api->set_param(inst, "seq_bd", "33825");
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
        api->set_param(inst, "seq_bd", "0");

        api->set_param(inst, "state", saved);
        api->get_param(inst, "sd_snappy", buf, sizeof(buf));
        CHECK(atoi(buf) == 17, "state restores sd_snappy");
        api->get_param(inst, "cy_level", buf, sizeof(buf));
        CHECK(atoi(buf) == 111, "state restores cy_level");
        api->get_param(inst, "hh_choke", buf, sizeof(buf));
        CHECK(atoi(buf) == 2, "state restores hh_choke");
        api->get_param(inst, "mutes", buf, sizeof(buf));
        CHECK(atoi(buf) == 5, "state restores mutes");
        api->get_param(inst, "seq_bd", buf, sizeof(buf));
        CHECK(atoi(buf) == 33825, "state restores the bd sequencer lane");
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

    /* ---- the built-in sequencer runs off the host transport ---- */
    api->set_param(inst, "mutes", "0");
    api->set_param(inst, "seq_bd", "1");                 /* step 0 only */
    for(int v = 1; v < 8; ++v)
    {
        char k[16]; snprintf(k, sizeof(k), "seq_%s", ids[v]);
        api->set_param(inst, k, "0");
    }
    g_beats = -1.0;
    render_peak(40);
    CHECK(render_peak(40) < 0.0005, "sequencer idle with no transport");
    g_beats = 0.0;
    {
        const double p = render_peak(20);
        char msg[96]; snprintf(msg, sizeof(msg),
            "sequencer fires step 0 when the transport starts (peak %.3f)", p);
        CHECK(p > 0.02, msg);
    }
    g_beats = -1.0;
    render_peak(900);

    api->destroy_instance(inst);
    CHECK(1, "destroy_instance");
    dlclose(h);

    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASS",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
