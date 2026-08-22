/*
 * sd606_cymbal.h — the voice the upstream repo does not ship.
 *
 * The real TR-606's roster is BD, SD, Low Tom, Hi Tom, CYMBAL, Open HH,
 * Closed HH. 606-Inspired-Synth-Drums covers six of those and adds an RD-6
 * clap, but there is no cymbal in it. This supplies one.
 *
 * On the hardware the cymbal and the hi-hats are the SAME metal source — one
 * oscillator bank split by different envelopes and filters — so the honest way
 * to build a cymbal here is a third HiHatSpec, not a new engine.
 * MetalHiHatVoice takes any spec; nothing in the vendored code changes.
 *
 * MEASURED, not designed. The table below was fitted from a hardware TR-606
 * cymbal recording by tools/fit_partials.py — FFTs at six points through the
 * decay, keeping the lines that survive it, which is the method Fecher
 * describes for the hats. Source: "Cymbal Clipped 606 Accent 4.wav"
 * (44.1 kHz, 24-bit, peak 0.50, no clipping despite the filename).
 *
 * The fit reaches down to 180 Hz. That matters: the vendored hat table stops
 * at 3804 Hz because it was measured off a HI-HAT, and a cymbal's body lives
 * below that. Re-voicing the hat table could never have produced it — which is
 * why this needed a real recording.
 *
 * Envelope fields are measured too (see the comments on each). The method was
 * checked against a control: fitting the OPEN HAT the same way gives a slow
 * decay of 0.540 s where Fecher's published spec says 0.580 s, and both agree
 * the fast component is nearly absent.
 *
 * GPL-3.0.
 */
#ifndef SD606_CYMBAL_H
#define SD606_CYMBAL_H

#include "HiHats.hpp"

namespace SynthDrums606 {

/* Fitted from hardware; see the header comment. 64 lines is kMaxPartialCount. */
static constexpr Partial kCymbalPartials[] = {
    {    180.1f,  0.067f },
    {    625.1f,  0.057f },
    {    802.0f,  0.103f },
    {   1149.8f,  0.089f },
    {   1204.8f,  0.354f },
    {   1256.4f,  0.066f },
    {   1395.5f,  0.113f },
    {   1546.7f,  0.046f },
    {   1588.7f,  0.052f },
    {   1634.2f,  0.078f },
    {   1716.8f,  0.198f },
    {   1804.1f,  0.055f },
    {   1890.4f,  0.285f },
    {   1994.9f,  0.075f },
    {   2094.9f,  0.047f },
    {   2197.7f,  0.049f },
    {   2313.9f,  0.258f },
    {   2447.6f,  0.391f },
    {   2574.2f,  0.192f },
    {   2705.3f,  0.073f },
    {   2854.4f,  0.353f },
    {   3004.4f,  0.064f },
    {   3146.7f,  0.546f },
    {   3304.2f,  0.354f },
    {   3477.0f,  0.209f },
    {   3666.3f,  1.000f, true },
    {   3855.4f,  0.047f },
    {   3966.9f,  0.050f },
    {   4084.3f,  0.358f },
    {   4296.2f,  0.052f },
    {   4504.8f,  0.182f },
    {   4732.7f,  0.091f },
    {   4865.3f,  0.201f },
    {   5011.6f,  0.149f },
    {   5301.7f,  0.161f },
    {   5580.5f,  0.427f },
    {   5872.4f,  0.346f },
    {   6201.0f,  0.869f, true },
    {   6376.3f,  0.296f },
    {   6560.2f,  0.292f },
    {   6906.0f,  0.814f },
    {   7090.3f,  0.057f },
    {   7285.8f,  1.000f, true },
    {   7677.7f,  0.427f },
    {   7883.4f,  0.206f },
    {   8091.4f,  0.344f },
    {   8522.6f,  0.643f },
    {   8979.2f,  0.300f },
    {   9447.9f,  0.161f },
    {   9953.2f,  0.208f },
    {  10216.3f,  0.050f },
    {  10490.7f,  0.125f },
    {  10773.4f,  0.062f },
    {  11067.9f,  0.208f },
    {  11653.8f,  0.127f },
    {  11951.0f,  0.047f },
    {  12268.8f,  0.155f },
    {  12924.1f,  0.144f },
    {  13620.4f,  0.192f },
    {  14350.7f,  0.112f },
    {  14743.6f,  0.050f },
    {  15155.1f,  0.076f },
    {  15985.5f,  0.095f },
    {  16700.1f,  0.068f },
};
static constexpr int kCymbalPartialCount = 64;

static constexpr HiHatSpec kCymbalSpec = {
    kCymbalPartials,
    kCymbalPartialCount,
    6000.0f,             // noiseHighPassHz      FITTED by resynthesis
    19000.0f,            // noiseLowPassHz       (these four together;
    0.110f,              // tonalMix             see the note below)
    1.300f,              // noiseMix
    0.800f,              // saturationDrive
                         //
                         // An FFT hands you partials and an envelope; it does
                         // not hand you the balance between metal and noise.
                         // So these four were searched (3000 voicings) against
                         // three measures of the hardware hit: 1/3-octave
                         // spectrum, decay envelope, and WITHIN-BAND CREST.
                         //
                         // The crest term is not decoration. Scored on spectrum
                         // alone the search walked tonalMix to zero — coarse
                         // band energies cannot tell 64 resonant lines from
                         // filtered noise of the same gross shape, so it was
                         // free to delete the metal and still "improve". Crest
                         // sees the lines.
                         //
                         // Result vs the same measurement of Fecher's SHIPPED
                         // open-hat spec against a real open hat (the yardstick
                         // for "as good as what already exists"):
                         //     spectrum  3.28 dB/band  vs  4.81
                         //     envelope  4.29 dB       vs  4.28
                         //     crest     3.55 dB       vs  4.01
                         // Interior optimum: it held with every axis extended
                         // past it. Note tonalMix landed on 0.110 — exactly the
                         // vendored hats' value — which is what you would hope
                         // for, the two voices being the same circuit family.
    1.30f,               // outputTrim
    0.0006f,             // attackTimeConstantSeconds
    0.22f,               // clickAmount — less click than a hat
    0.004f,              // clickDecaySeconds
    0.55f,               // bellAccentAmount — the ting is the cymbal's whole
    0.120f,              // bellAccentDecaySeconds   identity, so it lingers
    0.060f,              // envelopeFastWeight — MEASURED: the two-exponential
                         // fit puts almost everything in the slow term
    0.018f,              // fastDecaySeconds — MEASURED off the un-accented hit
    0.280f,              // slowDecaySeconds — MEASURED (accented hit; residual
                         // 0.079, and -60 dB at 1.86 s matches 0.28*ln(1000))
    true,                // decayScalesTimeConstants — the knob shortens the
                         // actual ring, not just the gate
    2.3342f,             // referenceDurationSeconds — MEASURED file length
                         // after onset (102939 frames / 44100)
    0.006f,              // minimumDecaySeconds
    0.070f,              // minimumDurationSeconds
    0.650f,              // gateFadeMaxSeconds — long, or the tail clicks off
    0.0011f,             // lineWobbleDepth — more drift than a hat; it is what
                         // stops a 2 s metal tail turning into a chord
    0.026f,              // lineWobbleCorrelationSeconds
};

} // namespace SynthDrums606

#endif /* SD606_CYMBAL_H */
