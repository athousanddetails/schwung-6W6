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

/*
 * Distortion flavours. Type 0 is the authentic diode rounding; the rest are
 * deliberate extensions -- the point of doing this in software rather than
 * cloning the circuit exactly.
 *
 * Maths ported verbatim from 9W9's er99_shape_st (GPL-3.0), where these were
 * tuned and approved by ear. Do not re-derive them.
 *
 *   0 Diode  the machine's own back-to-back diode rounding
 *   1 Clip   asymmetric soft clip, even harmonics
 *   2 SAT    warm parallel saturation, keeps the transient
 *   3 BFZ    thick fuzz wall
 *   4 PDIST  biased cubic crunch
 *   5 Fold   wavefolder            (was type 2 before v2 -- see the migration
 *   6 Crush  quantise AND decimate  (was type 3)  in sd606_engine.cpp)
 *
 * `_st` is two floats of state (held sample, decimator phase), needed only by
 * Crush. Pass null and Crush quantises without decimating; sd606_shape() is
 * that wrapper, kept so stateless call sites still compile.
 *
 * Every type is transparent at minimum drive -- a design rule, not an
 * accident, and why several branches divide by k below unity.
 */
static inline float sd606_shape_st(const float _x, const float _drive,
                                   const int _type, float *_st)
{
    const float k = _drive > 0.01f ? _drive : 0.01f;
    switch(_type)
    {
    case 1: {   /* asymmetric soft clip -- amp-like, even harmonics and all */
        const float bias = 0.35f, tb = 0.33638f;   /* tanhf(0.35f) */
        const float v = (tanhf(_x * k + bias) - tb) * 0.958f;
        return k < 1.0f ? v / k : v;
    }
    case 2: {   /* SAT -- warm, parallel, keeps the transient */
        const float u = _x * k + 0.08f * k * _x * _x;
        const float wet = u / (1.0f + fabsf(u));
        const float m = k < 1.0f ? k : 1.0f;    /* dry blend below unity */
        const float v = (1.0f - 0.65f * m) * _x * (k < 1.0f ? k : 1.0f)
                      + 0.65f * m * wet * 1.35f;
        return k < 1.0f ? v / k : v;
    }
    case 3: {   /* BFZ -- thick fuzz wall */
        /* The 2.5x pre-gain fuzzes even at minimum drive, so the fuzz is
         * crossfaded in between 0.85 and 2 -- below that the stage passes
         * dry, because drive-at-zero-is-transparent holds here too. */
        const float g = k * 2.5f;
        const float u = _x * g + 0.22f;
        const float wet = (u / (1.0f + fabsf(u)) - 0.18033f) * 1.05f;
        float m = (k - 0.85f) / 1.15f;
        if(m < 0.0f) m = 0.0f;
        if(m > 1.0f) m = 1.0f;
        return (1.0f - m) * _x + m * wet;
    }
    case 4: {   /* PDIST -- biased cubic crunch */
        float u = _x * k + 0.12f;
        if(u >  1.0f) u =  1.0f;
        if(u < -1.0f) u = -1.0f;
        const float y0 = 0.12f - (0.12f*0.12f*0.12f)/3.0f;   /* silence -> 0 */
        /* 1.5/1.479 makes the small-signal gain exactly k, so minimum drive
         * is transparent and the curve continuous -- the /k trick the other
         * types use would boost this one +3.4 dB at the knee. */
        return ((u - u*u*u/3.0f) - y0) * (1.5f / 1.479f);
    }
    case 5: {   /* wavefolder -- metallic, without hollowing the note out */
        float v = _x * k;
        for(int i = 0; i < 3; ++i)
        {
            if(v >  1.0f) v =  2.0f - v;
            if(v < -1.0f) v = -2.0f - v;
        }
        float body = _x * k;
        if(body >  1.0f) body =  1.0f;
        if(body < -1.0f) body = -1.0f;
        v = 0.62f * v + 0.38f * body;
        return k < 1.0f ? v / k : v;
    }
    case 6: {   /* bitcrush -- quantise + decimate */
        /* Depth falls and the hold stretches together as drive rises: from
         * (transparent, full rate) to (~3 levels, ~2.2 kHz at 44k1). */
        const float steps = 1.5f + 9.0f / k;
        float q = floorf(_x * steps + 0.5f) / steps;
        if(_st)
        {
            const float hold = k < 1.0f ? 1.0f : 1.0f + (k - 1.0f) * 1.7f;
            _st[1] += 1.0f;                 /* phase */
            if(_st[1] >= hold) { _st[1] -= hold; _st[0] = q; }
            q = _st[0];
        }
        return q;
    }
    case 0:
    default:
        return sd606_diode_round(_x, k);
    }
}

/* Stateless wrapper: Crush quantises but does not decimate. */
static inline float sd606_shape(const float _x, const float _drive, const int _type)
{
    return sd606_shape_st(_x, _drive, _type, (float *)0);
}

#endif /* SD606_SHAPE_H */
