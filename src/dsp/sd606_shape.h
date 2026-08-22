/*
 * sd606_shape.h — the post-voice drive stage.
 *
 * The vendored 606 voices have no distortion of their own beyond a little
 * internal saturation (the kick's soft-clip, the hat's tanh). Everything the
 * panel calls "Drive" and "Distortion" is this file, and it is deliberately
 * the same four flavours 9W9 uses so the two kits respond identically to the
 * same knob.
 *
 * Ported from 9W9's er99_circuit.h. GPL-3.0.
 */
#ifndef SD606_SHAPE_H
#define SD606_SHAPE_H

#include <math.h>

/*
 * Back-to-back diode rounding. Sharp peaks are what make a raw oscillator
 * sound buzzy; the diodes conduct near the peaks and round them off. A tanh
 * soft-clip is the standard model of that pair, normalised so unity drive
 * leaves the level alone.
 */
static inline float sd606_diode_round(const float _x, const float _drive)
{
    const float k = _drive > 0.01f ? _drive : 0.01f;
    return tanhf(k * _x) / tanhf(k);
}

/* 0 diode, 1 hard clip, 2 wavefolder, 3 bitcrush. */
static inline float sd606_shape(const float _x, const float _drive, const int _type)
{
    const float k = _drive > 0.01f ? _drive : 0.01f;
    switch(_type)
    {
    case 1: {   /* hard clip — aggressive, square-ish */
        float v = _x * k;
        if(v >  1.0f) v =  1.0f;
        if(v < -1.0f) v = -1.0f;
        return v;
    }
    case 2: {   /* wavefolder — metallic, odd harmonics rise with drive */
        float v = _x * k;
        for(int i = 0; i < 3; ++i)
        {
            if(v >  1.0f) v =  2.0f - v;
            if(v < -1.0f) v = -2.0f - v;
        }
        return v;
    }
    case 3: {   /* bitcrush / decimate — lo-fi grit */
        const float steps = 2.0f + 30.0f / k;
        return floorf(_x * steps + 0.5f) / steps;
    }
    case 0:
    default:
        return sd606_diode_round(_x, k);
    }
}

#endif /* SD606_SHAPE_H */
