/*
 * clap_equiv.cpp — the forked clap must be the vendored clap, sample for
 * sample, and cheaper. Run on the device.
 *
 * "Without losing the sound it has now" is only worth claiming if it is
 * checked at the strongest level available: not a spectral distance, not an
 * RMS tolerance — every output float bit-identical, across the whole
 * parameter surface.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <vector>

#include "Clap.hpp"              // vendored
#include "sd606_clap_voice.h"    // fork

using namespace SynthDrums606;
static double now(){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
                    return t.tv_sec+t.tv_nsec*1e-9;}
static int fails = 0;
#define CHECK(c,m) do{ if(!(c)){ printf("FAIL: %s\n", m); fails++; } \
                       else printf("ok  : %s\n", m); }while(0)

int main()
{
    const double sr = 44100.0;
    const int N = (int)(sr * 0.35);      /* the clap is 301 ms */

    /* ---- bit-exactness across the parameter surface ---- */
    const float decays[]  = { 0.05f, 0.4f, 0.8f, 1.0f };
    const float ratios[]  = { 0.5f, 0.8f, 1.0f, 1.5f, 2.0f };   /* <1 engages the upsampler */
    const float noises[]  = { 0.0f, 0.25f, 0.5f, 0.504f, 0.75f, 1.0f };

    long compared = 0; double worst = 0.0; int mismatches = 0;
    for(float d : decays) for(float r : ratios) for(float n : noises)
    {
        ClapVoice a;       a.init(sr, 0x0606C1A9u);
        Sd606ClapVoice b;  b.init(sr, 0x0606C1A9u);
        a.trigger(d, r, n);
        b.trigger(d, r, n);
        for(int i = 0; i < N; ++i)
        {
            const float x = a.process(), y = b.process();
            ++compared;
            if(memcmp(&x, &y, sizeof(float)) != 0)
            {
                ++mismatches;
                const double e = fabs((double)x - (double)y);
                if(e > worst) worst = e;
            }
        }
    }
    char msg[160];
    snprintf(msg, sizeof msg,
             "bit-identical over %ld samples x %d settings (%d mismatches, worst %.3g)",
             compared, (int)(sizeof(decays)/4 * sizeof(ratios)/4 * sizeof(noises)/4),
             mismatches, worst);
    CHECK(mismatches == 0, msg);

    /* retrigger mid-tail: the ring buffer state must match too */
    {
        ClapVoice a; a.init(sr, 0x0606C1A9u);
        Sd606ClapVoice b; b.init(sr, 0x0606C1A9u);
        int bad = 0;
        for(int k = 0; k < 6; ++k)
        {
            a.trigger(0.8f, 1.0f, 0.504f);
            b.trigger(0.8f, 1.0f, 0.504f);
            for(int i = 0; i < (int)(sr * 0.08); ++i)   /* cut the tail short */
            {
                const float x = a.process(), y = b.process();
                if(memcmp(&x, &y, sizeof(float)) != 0) ++bad;
            }
        }
        CHECK(bad == 0, "bit-identical when retriggered mid-tail");
    }

    /* ---- cost ---- */
    const int BN = (int)(sr * 4);
    const int period = (int)(sr / 8);
    double ta, tb;
    volatile float sink = 0;
    { ClapVoice v; v.init(sr, 0x0606C1A9u); double t = now();
      for(int i = 0; i < BN; ++i){ if(i % period == 0) v.trigger(0.8f, 1.0f, 0.504f); sink = v.process(); }
      ta = now() - t; }
    { Sd606ClapVoice v; v.init(sr, 0x0606C1A9u); double t = now();
      for(int i = 0; i < BN; ++i){ if(i % period == 0) v.trigger(0.8f, 1.0f, 0.504f); sink = v.process(); }
      tb = now() - t; }
    (void)sink;
    const double audio = BN / sr;
    printf("\n  vendored : %7.1f ms  = %5.2f%% of realtime\n", ta*1000, 100*ta/audio);
    printf("  forked   : %7.1f ms  = %5.2f%% of realtime   (%.2fx faster)\n",
           tb*1000, 100*tb/audio, ta/tb);
    CHECK(tb < ta * 0.85, "the fork is at least 15% cheaper");

    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASS", fails,
           fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
