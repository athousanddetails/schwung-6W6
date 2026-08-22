/*
 * sd606_metal.h — trimmed hi-hat partial tables.
 *
 * MetalHiHatVoice sums one sin() per partial per sample, and it is by a wide
 * margin the most expensive thing in this module: on the Move each metal voice
 * cost ~15% of the block budget against ~6.5% for an analog one, and three of
 * them can ring at once.
 *
 * The vendored hats use 47 lines. Scoring trimmed versions against a hardware
 * open-hat recording (tools/cymbal_tune.cpp --openhat --partials N) shows no
 * measurable cost down to 16:
 *
 *     47 lines: spectral 3.72 dB/band, crest 3.97 dB
 *     32 lines: spectral 3.64 dB/band, crest 3.82 dB
 *     24 lines: spectral 3.80 dB/band, crest 3.92 dB
 *     16 lines: spectral 3.75 dB/band, crest 3.88 dB
 *
 * That is flat inside the noise. 24 is chosen rather than 16 because the
 * metric is a proxy — it can see spectrum and peakiness, not density — so the
 * cheapest option is not automatically the right one.
 *
 * The 24 loudest lines of the vendored table, kept in frequency order. The
 * three bell lines survive by construction; they are the loudest in it.
 * Nothing under src/vendor is modified: these specs are copies that point at
 * the shorter table.
 *
 * GPL-3.0. Frequencies and amplitudes are Fecher's measurements (MIT).
 */
#ifndef SD606_METAL_H
#define SD606_METAL_H

#include "HiHats.hpp"

namespace SynthDrums606 {

static constexpr Partial kHatPartialsLite[] = {
    {   3804.0f,  0.072f },
    {   4270.0f,  0.077f },
    {   4900.0f,  0.099f },
    {   5400.0f,  0.116f },
    {   5500.0f,  0.116f },
    {   5610.0f,  0.088f },
    {   5978.0f,  0.742f, true },
    {   6340.0f,  1.300f, true },
    {   6500.0f,  0.181f },
    {   6600.0f,  0.178f },
    {   7106.0f,  0.126f },
    {   7200.0f,  0.128f },
    {   7500.0f,  0.132f },
    {   7686.0f,  0.573f, true },
    {   7854.0f,  0.086f },
    {   8048.0f,  0.062f },
    {   8399.0f,  0.078f },
    {   8514.0f,  0.081f },
    {   8876.0f,  0.166f },
    {   9100.0f,  0.081f },
    {   9394.0f,  0.101f },
    {   9600.0f,  0.064f },
    {  10143.0f,  0.062f },
    {  12680.0f,  0.090f },
};
static constexpr int kHatPartialsLiteCount = 24;

inline HiHatSpec sd606_lite(HiHatSpec spec)
{
    spec.partials     = kHatPartialsLite;
    spec.partialCount = kHatPartialsLiteCount;
    return spec;
}

} // namespace SynthDrums606

#endif /* SD606_METAL_H */
