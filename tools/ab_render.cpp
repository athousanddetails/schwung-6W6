/* Renders a fixed 4-second pattern at default pots to raw float32.
 * Used by tools/ab_null.sh against two builds of the engine. */
#include <stdio.h>
#include <stdlib.h>
#include "sd606_engine.h"
int main(int argc, char **argv)
{
    const float sr = 44100.0f;
    sd606_engine_t *e = sd606_create(sr);
    /* AB_VEL_DEPTH=0 reproduces the pre-velocity behaviour, so the two
     * builds can be compared on equal terms. */
    { const char *d = getenv("AB_VEL_DEPTH"); if(d) sd606_set_param(e, "vel_depth", d); }
    const char *pat[SD606_NUM_VOICES] = {
        "X..x..X...x..X..", "....X.......X...", "..............x.",
        "..........x.....", "x.x.x.x.x.x.x.x.", "......x.......x.",
        "X...............", "......x........." };
    FILE *f = fopen(argc > 1 ? argv[1] : "ab.f32", "wb");
    float blk[128];
    int last = -1;
    for(int b = 0; b < (int)(sr * 4) / 128; ++b)
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
        fwrite(blk, 4, 128, f);
    }
    fclose(f);
    sd606_destroy(e);
    return 0;
}
