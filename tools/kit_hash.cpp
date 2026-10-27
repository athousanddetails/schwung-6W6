/*
 * kit_hash.cpp — the "don't break what we have" proof.
 *
 * 6W6's kit was fitted against hardware. Any change that is supposed to leave
 * it alone must leave it EXACTLY alone, and the only honest way to say that is
 * to hash the float output and compare. "It sounds the same" is not a proof.
 *
 * Prints a PER-VOICE running hash, not just a final one: when a port diverges,
 * the per-voice split names the first affected voice in a single run.
 *
 *   ./kit_hash            # every voice at default pots, 1 s each, + a pattern
 */
#include <stdio.h>
#include <string.h>
#include "sd606_engine.h"

static unsigned h = 2166136261u;                  /* FNV-1a over the raw bits */
static void feed(const float *o, int n)
{
    for(int i = 0; i < n; ++i)
    {
        union { float f; unsigned u; } b;
        b.f = o[i];
        h = (h ^ b.u) * 16777619u;
    }
}

int main(void)
{
    const float sr = 44100.0f;
    float blk[128];

    for(int v = 0; v < SD606_NUM_VOICES; ++v)
    {
        sd606_engine_t *e = sd606_create(sr);
        sd606_trigger(e, v, 110);
        for(int b = 0; b < 44100 / 128; ++b) { sd606_render(e, blk, 128); feed(blk, 128); }
        sd606_destroy(e);
        printf("  %-3s %08x\n", sd606_voice_id(v), h);
    }

    /* a pattern, so voice interaction and the master stage are covered too */
    {
        sd606_engine_t *e = sd606_create(sr);
        const char *pat[SD606_NUM_VOICES] = {
            "X..x..X...x..X..", "....X.......X...", "..............x.",
            "..........x.....", "x.x.x.x.x.x.x.x.", "......x.......x.",
            "X...............", "................" };
        int last = -1;
        for(int b = 0; b < 44100 * 4 / 128; ++b)
        {
            const double t = (double)b * 128.0 / sr;
            const int step = (int)(t / (60.0 / 132.0 / 4.0)) % 16;
            if(step != last)
            {
                last = step;
                for(int v = 0; v < SD606_NUM_VOICES; ++v)
                {
                    const char c = pat[v][step];
                    if(c == 'x') sd606_trigger(e, v, 90);
                    if(c == 'X') sd606_trigger(e, v, 120);
                }
            }
            sd606_render(e, blk, 128);
            feed(blk, 128);
        }
        sd606_destroy(e);
        printf("  pattern %08x\n", h);
    }
    printf("KIT %08x\n", h);
    return 0;
}
