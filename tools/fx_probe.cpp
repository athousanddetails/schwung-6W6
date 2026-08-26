/*
 * fx_probe.cpp — does the send FX do what it says?
 *
 * Four claims, each measured rather than eyeballed:
 *   1. the echo lands at the millisecond the division and tempo imply
 *   2. the reverb leaves energy where the dry voice is already silent
 *   3. the reverb decays to true silence at the default
 *   4. both highpasses measurably cut lows on the wet path
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include "sd606_engine.h"

static int fails = 0;
#define CHECK(c,m) do{ if(!(c)){ printf("FAIL: %s\n", m); fails++; } \
                       else printf("ok  : %s\n", m); }while(0)

static std::vector<float> render(sd606_engine_t *e, double seconds)
{
    std::vector<float> o((size_t)(44100 * seconds));
    float blk[128];
    for(size_t i = 0; i < o.size(); i += 128)
    {
        sd606_render(e, blk, 128);
        for(int k = 0; k < 128 && i + k < o.size(); ++k) o[i + k] = blk[k];
    }
    return o;
}
static void set(sd606_engine_t *e, const char *k, const char *v) { sd606_set_param(e, k, v); }

/* first sample after `fromMs` that exceeds `frac` of the global peak */
static double onset_ms(const std::vector<float> &o, double fromMs, double frac)
{
    float pk = 0; for(float v : o) pk = fmaxf(pk, fabsf(v));
    const size_t from = (size_t)(fromMs * 44.1);
    for(size_t i = from; i < o.size(); ++i)
        if(fabsf(o[i]) > pk * frac) return i / 44.1;
    return -1.0;
}
static double rms_between(const std::vector<float> &o, double aMs, double bMs)
{
    const size_t a = (size_t)(aMs * 44.1), b = (size_t)(bMs * 44.1);
    double s = 0; size_t n = 0;
    for(size_t i = a; i < b && i < o.size(); ++i) { s += o[i] * o[i]; ++n; }
    return n ? sqrt(s / n) : 0.0;
}

int main(void)
{
    char msg[160];

    /* ---- 1. delay time tracks the division and the tempo ---- */
    struct { const char *div; int idx; float bpm; double beats; } cases[] = {
        { "1/8",  5, 120.0f, 0.5 }, { "1/4",  8, 140.0f, 1.0 },
        { "1/8.", 7, 100.0f, 0.75 },
    };
    for(auto &c : cases)
    {
        sd606_engine_t *e = sd606_create(44100.0f);
        set(e, "volume", "127"); set(e, "master_dist", "0");
        set(e, "cy_level", "0"); set(e, "ch_level", "0");
        char b[16]; snprintf(b, sizeof b, "%d", c.idx);
        set(e, "dly_time", b);
        snprintf(b, sizeof b, "%.4f", c.bpm); set(e, "dly_bpm", b);
        set(e, "bd_dly", "127"); set(e, "dly_fdbk", "0"); set(e, "dly_level", "127");
        set(e, "dly_hpf", "0");                       /* let the kick through */
        sd606_trigger(e, SD606_BD, 110);
        std::vector<float> o = render(e, 3.0);
        const double want = c.beats * 60000.0 / c.bpm;
        const double got  = onset_ms(o, want * 0.55, 0.02);
        snprintf(msg, sizeof msg, "%-5s at %5.0f BPM: echo at %7.1f ms, theory %7.1f",
                 c.div, c.bpm, got, want);
        CHECK(got > 0 && fabs(got - want) < want * 0.03, msg);
        sd606_destroy(e);
    }

    /* ---- 2 + 3. reverb tail, then true silence ----
     * Against a CLOSED HAT (166 ms), not the snare: at default decay the comb
     * loop is ~25 ms with 0.62 feedback, so the tail is ~0.4 s. Measured
     * against a 2 s source it would look like nothing at all. */
    {
        sd606_engine_t *dry = sd606_create(44100.0f);
        set(dry, "volume", "127"); set(dry, "master_dist", "0");
        sd606_trigger(dry, SD606_CH, 110);
        std::vector<float> d = render(dry, 3.0);
        sd606_destroy(dry);

        sd606_engine_t *wet = sd606_create(44100.0f);
        set(wet, "volume", "127"); set(wet, "master_dist", "0");
        set(wet, "ch_rev", "127");
        sd606_trigger(wet, SD606_CH, 110);
        std::vector<float> w = render(wet, 3.0);
        sd606_destroy(wet);

        const double dTail = rms_between(d, 220, 420);
        const double wTail = rms_between(w, 220, 420);
        snprintf(msg, sizeof msg,
                 "reverb rings where the dry hat is dead (dry %.2e, wet %.2e)",
                 dTail, wTail);
        CHECK(wTail > dTail * 10.0 && wTail > 1e-5, msg);

        const double end = rms_between(w, 2500, 3000);
        snprintf(msg, sizeof msg, "and decays to true silence by 2.5 s (%.2e)", end);
        CHECK(end < 1e-7, msg);
    }

    /* ---- 4. the send highpasses cut lows ----
     * The sends are POST-FADER, so the dry level cannot be zeroed to isolate
     * the wet -- that would send silence. Instead render twice, identically,
     * once with the send down and once up, and SUBTRACT: the engine is
     * deterministic, so the difference is exactly the wet signal. */
    for(int which = 0; which < 2; ++which)
    {
        const char *send = which ? "bd_dly" : "bd_rev";
        const char *hpf  = which ? "dly_hpf" : "rev_hpf";
        double e_lo[2];
        for(int hi = 0; hi < 2; ++hi)
        {
            std::vector<float> pass[2];
            for(int on = 0; on < 2; ++on)
            {
                sd606_engine_t *e = sd606_create(44100.0f);
                set(e, "volume", "127"); set(e, "master_dist", "0");
                set(e, hpf, hi ? "127" : "0");        /* 800 Hz vs 30 Hz */
                if(which) { set(e, "dly_fdbk", "0"); set(e, "dly_level", "127"); }
                set(e, send, on ? "127" : "0");
                sd606_trigger(e, SD606_BD, 110);
                pass[on] = render(e, 2.0);
                sd606_destroy(e);
            }
            double lp1 = 0, lp2 = 0, lp3 = 0, acc = 0;
            const double a = 1.0 - exp(-2.0 * M_PI * 120.0 / 44100.0);
            for(size_t i = 0; i < pass[0].size(); ++i)
            {
                const double v = pass[1][i] - pass[0][i];   /* the wet, exactly */
                lp1 += a * (v - lp1); lp2 += a * (lp1 - lp2); lp3 += a * (lp2 - lp3);
                acc += lp3 * lp3;
            }
            e_lo[hi] = sqrt(acc / pass[0].size());
        }
        snprintf(msg, sizeof msg, "%s cuts lows on the wet path: %.2e -> %.2e (%.1f dB)",
                 hpf, e_lo[0], e_lo[1], 20 * log10(e_lo[1] / (e_lo[0] + 1e-30)));
        CHECK(e_lo[1] < e_lo[0] * 0.5, msg);
    }

    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASS", fails,
           fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
