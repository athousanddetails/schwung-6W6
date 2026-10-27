/*
 * bench.cpp — the phase-1 CPU gate.
 *
 * Each metal voice (CH, OH, CY) sums 47 sines per sample, and all three can
 * ring at once. Everything built on top of this engine assumes that fits in
 * the Move's ~2.9 ms block budget, so the assumption gets measured on the
 * device before it gets built on.
 *
 * Cross-compiled for aarch64 and run ON the Move:
 *   ./sd606_bench
 *
 * Reports realtime factor per scenario. Anything under ~4x is a warning; the
 * SPI callback shares core 3 and a drum machine must never be the reason a
 * frame is late.
 */
#include <stdio.h>
#include <time.h>

#include "sd606_engine.h"

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* Retrigger every voice in `lanes` at `hz`, render `seconds`, report. */
static void run(const char *label, const int *lanes, int lane_count,
                double retrigger_hz, double seconds)
{
    const float sr = 44100.0f;
    const int   block = 128;
    sd606_engine_t *e = sd606_create(sr);
    if(!e) { printf("FAIL: sd606_create\n"); return; }

    float out[128];
    const long total_blocks = (long)(seconds * sr / block);
    const long retrigger_every =
        retrigger_hz > 0.0 ? (long)(sr / block / retrigger_hz) : 0;

    const double t0 = now_seconds();
    for(long b = 0; b < total_blocks; ++b)
    {
        if(retrigger_every > 0 && (b % retrigger_every) == 0)
            for(int i = 0; i < lane_count; ++i)
                sd606_trigger(e, lanes[i], 110);
        sd606_render(e, out, block);
    }
    const double elapsed = now_seconds() - t0;
    const double audio   = (double)total_blocks * block / sr;

    printf("%-38s %7.2f ms cpu / %6.2f s audio  =  %6.2fx realtime  "
           "(%5.1f%% of block budget)\n",
           label, elapsed * 1000.0, audio, audio / elapsed,
           100.0 * elapsed / audio);
    sd606_destroy(e);
}

int main(void)
{
    printf("6W6 CPU gate — 44100 Hz, 128-frame blocks\n\n");

    for(int v = 0; v < SD606_NUM_VOICES; ++v)
    {
        char label[64];
        snprintf(label, sizeof(label), "%s alone @ 8 hits/s", sd606_voice_id(v));
        const int lane[1] = { v };
        run(label, lane, 1, 8.0, 4.0);
    }

    printf("\n");
    const int metal[3] = { SD606_CH, SD606_OH, SD606_CY };
    run("3 metal voices @ 8 hits/s", metal, 3, 8.0, 4.0);

    const int all[SD606_NUM_VOICES] = {
        SD606_BD, SD606_SD, SD606_LT, SD606_HT,
        SD606_CH, SD606_OH, SD606_CY, SD606_CP
    };
    run("WHOLE KIT @ 8 hits/s", all, SD606_NUM_VOICES, 8.0, 4.0);
    run("WHOLE KIT @ 16 hits/s (worst case)", all, SD606_NUM_VOICES, 16.0, 4.0);
    return 0;
}
