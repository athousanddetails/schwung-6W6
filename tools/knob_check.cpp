/*
 * Does every control DO something?
 *
 * Nothing else in this repo asserts EFFECT. The loadtest checks ranges, names,
 * defaults, key resolution, storage order and migration -- and every one of
 * those passes with a knob wired to nothing. 8W8 shipped exactly that: a pot
 * that resolved a slot and reached no voice, dead at 0/64/127, with a green
 * suite. 6W6 has had pots deleted from the middle of its table twice and one
 * appended back, so it is the same surgery on the same kind of table.
 *
 * Method: render the control at both ends of its range and hash the audio. If
 * the hashes match, the control changed nothing.
 *
 * The hard part is not the hash, it is the CONTEXT. A distortion TYPE does
 * nothing while Drive is 0 -- and Drive defaults to 0. A send does nothing
 * while the reverb is silent. vel_depth does nothing at velocity 127, because
 * full velocity is the top of the range by definition. Each control is
 * therefore measured where it is SUPPOSED to work, and that context is a
 * design claim: if a control needs a context to matter, say which.
 *
 * Proven to fail, which is the only reason to trust a green run: stubbing the
 * snare so sd_snappy still resolves and still stores but never reaches the
 * voice makes this report `DEAD: sd_snappy` -- while src/tools/loadtest.c
 * reports ALL PASS against the very same build. That gap is the whole point.
 *
 *   clang++ -std=c++14 -O2 -Isrc -Isrc/dsp -Isrc/host -Isrc/vendor/606 \
 *       tools/knob_check.cpp src/dsp/sd606_engine.cpp -o /tmp/knob_check
 *
 * Idea and method from the CW-78 session's tools/knob_check.cpp.
 */
#include "sd606_engine.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

static const char *kVoice[8] = {"bd","sd","lt","ht","ch","oh","cy","cp"};

struct Ctx { const char *key; const char *val; };

/* Render `frames` after triggering `voices`, hashing the output. */
static uint64_t run(const std::vector<Ctx> &ctx, const char *key, const char *val,
                    const std::vector<int> &voices, int vel, int frames)
{
    sd606_engine_t *e = sd606_create(44100.0f);
    for (const Ctx &c : ctx) sd606_set_param(e, c.key, c.val);
    sd606_set_param(e, key, val);
    for (int v : voices) sd606_trigger(e, v, vel);
    uint64_t h = 1469598103934665603ull;
    float buf[64];
    for (int i = 0; i < frames; i += 64) {
        memset(buf, 0, sizeof buf);
        sd606_render(e, buf, 64);
        const unsigned char *p = (const unsigned char *)buf;
        for (size_t b = 0; b < sizeof buf; ++b) { h ^= p[b]; h *= 1099511628211ull; }
    }
    sd606_destroy(e);
    return h;
}

/* Some controls only act on a voice that is ALREADY RINGING -- the hat choke
 * is the case: CH cuts a sounding OH. Striking both in the same sample gives
 * the choke nothing to cut and it reads as dead. */
static uint64_t runSeq(const std::vector<Ctx> &ctx, const char *key, const char *val,
                       int first, int second, int gapFrames, int frames)
{
    sd606_engine_t *e = sd606_create(44100.0f);
    for (const Ctx &c : ctx) sd606_set_param(e, c.key, c.val);
    sd606_set_param(e, key, val);
    sd606_trigger(e, first, 127);
    uint64_t h = 1469598103934665603ull;
    float buf[64];
    for (int i = 0; i < frames; i += 64) {
        if (i >= gapFrames && i - 64 < gapFrames) sd606_trigger(e, second, 127);
        memset(buf, 0, sizeof buf);
        sd606_render(e, buf, 64);
        const unsigned char *p = (const unsigned char *)buf;
        for (size_t b = 0; b < sizeof buf; ++b) { h ^= p[b]; h *= 1099511628211ull; }
    }
    sd606_destroy(e);
    return h;
}

static int fails = 0, checked = 0;
static void check(const char *key, const char *lo, const char *hi,
                  const std::vector<Ctx> &ctx, const std::vector<int> &voices,
                  int vel, const char *why)
{
    const int frames = 44100 * 2;                 /* 2 s: long enough for a delay tail */
    uint64_t a = run(ctx, key, lo, voices, vel, frames);
    uint64_t b = run(ctx, key, hi, voices, vel, frames);
    ++checked;
    if (a == b) { printf("DEAD: %-14s %s..%s  (%s)\n", key, lo, hi, why); ++fails; }
    else        printf("ok  : %-14s %s..%s\n", key, lo, hi);
}

int main(void)
{
    std::vector<Ctx> none;
    const std::vector<int> all = {0,1,2,3,4,5,6,7};

    /* ---- per-voice controls: trigger that voice, nothing else ---- */
    for (int v = 0; v < 8; ++v) {
        const std::string id = kVoice[v];
        const std::vector<int> one = { v };
        for (const char *suf : {"_tune","_decay","_attack","_snappy","_tone","_noise","_level"}) {
            const std::string k = id + suf;
            char buf[8];
            sd606_engine_t *probe = sd606_create(44100.0f);
            const bool exists = sd606_get_param(probe, k.c_str(), buf, sizeof buf) > 0;
            sd606_destroy(probe);
            if (!exists) continue;
            check(k.c_str(), "0", "127", none, one, 127, "plain");
        }
        /* Drive needs nothing; the TYPE needs drive up, or it shapes silence. */
        check((id + "_drive").c_str(), "0", "127", none, one, 127, "plain");
        std::vector<Ctx> driven = {{ (new std::string(id + "_drive"))->c_str(), "127" }};
        check((id + "_dist_type").c_str(), "0", "6", driven, one, 127, "drive at 127");
        /* A send does nothing unless the bus it feeds is audible. */
        std::vector<Ctx> revup = {{"rev_level","127"}};
        std::vector<Ctx> dlyup = {{"dly_level","127"}};
        check((id + "_rev").c_str(), "0", "127", revup, one, 127, "rev_level 127");
        check((id + "_dly").c_str(), "0", "127", dlyup, one, 127, "dly_level 127");
    }

    /* ---- the FX pages: need something sent into them ---- */
    std::vector<Ctx> intoRev = {{"bd_rev","127"},{"sd_rev","127"},{"rev_level","127"}};
    for (const char *k : {"rev_decay","rev_tone","rev_hpf","rev_level"})
        check(k, "0", "127", intoRev, {0,1}, 127, "bd+sd sent to reverb");
    std::vector<Ctx> intoDly = {{"bd_dly","127"},{"sd_dly","127"},{"dly_level","127"}};
    for (const char *k : {"dly_fdbk","dly_tone","dly_hpf","dly_level"})
        check(k, "0", "127", intoDly, {0,1}, 127, "bd+sd sent to delay");
    check("dly_time", "0", "12", intoDly, {0,1}, 127, "bd+sd sent to delay");

    /* ---- master ---- */
    check("volume", "0", "127", none, all, 127, "whole kit");
    check("comp",   "0", "127", none, all, 127, "whole kit");
    /* master_drive is gated on a TYPE being chosen -- `if(mdist > 0 && mpot > 0)`
     * -- and master_dist defaults to Off, so with no type it is correctly
     * inert. Same shape as a voice's Drive vs its Distortion type. */
    std::vector<Ctx> mtyped = {{"master_dist","1"}};
    check("master_drive", "0", "127", mtyped, all, 127, "master_dist = Diode");
    std::vector<Ctx> mdriven = {{"master_drive","127"}};
    check("master_dist", "1", "7", mdriven, all, 127, "master_drive 127");
    /* vel_depth only carves BELOW full velocity: at 127 it is a no-op by design. */
    check("vel_depth", "0", "127", none, all, 40, "velocity 40, not 127");
    /* Choke needs a RINGING open hat to cut: strike OH, let it ring 100 ms,
     * then strike CH. Striking both at once gives it nothing to act on. */
    {
        const int frames = 44100 * 2, gap = 4410;
        uint64_t a = runSeq(none, "hh_choke", "0", 5, 4, gap, frames);
        uint64_t b = runSeq(none, "hh_choke", "1", 5, 4, gap, frames);
        ++checked;
        if (a == b) { printf("DEAD: %-14s 0..1  (OH ringing, then CH)\n", "hh_choke"); ++fails; }
        else        printf("ok  : %-14s 0..1\n", "hh_choke");
    }

    printf("\n%d controls checked, %d dead\n", checked, fails);
    return fails ? 1 : 0;
}
