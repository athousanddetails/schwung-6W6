/*
 * render.cpp — offline renderer. Writes a WAV so the kit can be heard (and
 * A/B'd against hardware) without a device in the loop.
 *
 *   ./sd606_render out.wav [pattern]
 *     pattern 0 = one hit per voice, spaced, in lane order  (voice audition)
 *     pattern 1 = a 606-ish two-bar pattern at 132 BPM      (kit in context)
 *     pattern 2 = hat choke demo: OH on every 8th, CH between them
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sd606_engine.h"

static void put32(FILE *f, unsigned v) { fputc(v & 255, f); fputc((v >> 8) & 255, f);
                                         fputc((v >> 16) & 255, f); fputc((v >> 24) & 255, f); }
static void put16(FILE *f, unsigned v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); }

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "6w6.wav";
    const int   pattern = argc > 2 ? atoi(argv[2]) : 1;
    const float sr = 44100.0f;

    sd606_engine_t *e = sd606_create(sr);
    if(!e) { fprintf(stderr, "sd606_create failed\n"); return 1; }

    /* Pattern 2 needs the choke on; pattern 1 leaves defaults alone. */
    if(pattern == 2) sd606_set_param(e, "hh_choke", "1");

    const int   block = 128;
    const double bpm = 132.0;
    const double step_seconds = 60.0 / bpm / 4.0;     /* 16th notes */
    const long  total_frames = (long)(sr * (pattern == 0 ? 9.0 : 8.0));

    /* x = hit, X = accented hit. 32 steps = two bars of 16ths. */
    static const char *kPattern[SD606_NUM_VOICES] = {
        /* bd */ "X..x..X...x..X..X..x..X...x..X..",
        /* sd */ "....X.......X.......X.......X...",
        /* lt */ "..............x...............x.",
        /* ht */ "..........x...............x.....",
        /* ch */ "x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.",
        /* oh */ "......x.......x.......x.......x.",
        /* cy */ "X...............................",
        /* cp */ "................................",
    };
    static const char *kChoke[SD606_NUM_VOICES] = {
        "X.......X.......X.......X.......",
        "................................",
        "................................",
        "................................",
        /* ch */ "..x...x...x...x...x...x...x...x.",
        /* oh */ "x...x...x...x...x...x...x...x...",
        "................................",
        "................................",
    };
    const char *const *pat = (pattern == 2) ? kChoke : kPattern;

    FILE *f = fopen(path, "wb");
    if(!f) { fprintf(stderr, "cannot write %s\n", path); sd606_destroy(e); return 1; }
    const long data_bytes = total_frames * 2 * 2;     /* stereo int16 */
    fwrite("RIFF", 1, 4, f); put32(f, (unsigned)(36 + data_bytes));
    fwrite("WAVEfmt ", 1, 8, f); put32(f, 16); put16(f, 1); put16(f, 2);
    put32(f, (unsigned)sr); put32(f, (unsigned)(sr * 4)); put16(f, 4); put16(f, 16);
    fwrite("data", 1, 4, f); put32(f, (unsigned)data_bytes);

    float mono[128];
    long frame = 0;
    int  last_step = -1;
    float peak = 0.0f;
    long  nonzero = 0;

    while(frame < total_frames)
    {
        if(pattern == 0)
        {
            /* One voice per second, in lane order. */
            const int slot = (int)(frame / (long)sr);
            const int at   = (int)(frame % (long)sr);
            if(at < block && slot < SD606_NUM_VOICES)
                sd606_trigger(e, slot, 110);
        }
        else
        {
            const int step = (int)((double)frame / sr / step_seconds) % 32;
            if(step != last_step)
            {
                last_step = step;
                for(int v = 0; v < SD606_NUM_VOICES; ++v)
                {
                    const char c = pat[v][step];
                    if(c == 'x') sd606_trigger(e, v, 90);
                    if(c == 'X') sd606_trigger(e, v, 120);   /* accented */
                }
            }
        }

        sd606_render(e, mono, block);
        for(int i = 0; i < block && frame < total_frames; ++i, ++frame)
        {
            float v = mono[i];
            if(v > peak) peak = v;
            if(v < -peak) peak = -v;
            if(v != 0.0f) ++nonzero;
            if(v >  1.0f) v =  1.0f;
            if(v < -1.0f) v = -1.0f;
            const short s = (short)(v * 32767.0f);
            put16(f, (unsigned short)s);
            put16(f, (unsigned short)s);
        }
    }
    fclose(f);
    sd606_destroy(e);

    printf("%s: %ld frames, peak %.3f, %ld non-silent samples (%.1f%%)\n",
           path, total_frames, peak, nonzero, 100.0 * nonzero / total_frames);
    if(peak < 0.01f)  { fprintf(stderr, "FAIL: output is silent\n"); return 1; }
    if(peak > 0.999f) { fprintf(stderr, "WARN: output is clipping\n"); }
    return 0;
}
