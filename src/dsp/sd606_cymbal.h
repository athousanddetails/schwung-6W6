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
 * MEASURED from hardware. Source: "Cymbal 606 Clean.wav" — the UN-ACCENTED
 * hit, 44.1 kHz 24-bit, crest 18.6 dB. The accented take was rejected as a
 * fitting source: accent drives the voice harder, so any distortion it adds
 * would be baked into this table as if it were a resonance of the instrument.
 * A fit wants the cleanest capture of the circuit, not the loudest.
 *
 * The fit reaches down to 266 Hz. That matters: the vendored hat table stops
 * at 3804 Hz because it was measured off a HI-HAT, and a cymbal's body lives
 * below that. Re-voicing the hat table could never have produced it.
 *
 * 32 lines, not the 64 the fit produces. Partial count is this engine's
 * dominant CPU cost — one sin() per line per sample — and a measured sweep
 * against this same recording found quality FLAT from 64 down to 24
 * (spectral 3.77 -> 4.00 dB/band) and only breaking at 16. 32 takes half the
 * CPU with margin over the knee. Re-run the sweep before changing it:
 *   ./cymbal_tune "Cymbal 606 Clean.wav" --partials N
 *
 * Partials and envelope come from tools/fit_partials.py — FFTs at six points
 * through the decay, keeping the lines that survive it, which is the method
 * Fecher describes for the hats. The voicing fields come from
 * tools/cymbal_tune.cpp, which renders the voice and scores it against the
 * recording. Both are re-runnable; neither is a taste judgement.
 *
 * GPL-3.0.
 */
#ifndef SD606_CYMBAL_H
#define SD606_CYMBAL_H

#include "HiHats.hpp"

namespace SynthDrums606 {

static constexpr Partial kCymbalPartials[] = {
    {   1202.2f,  0.315f },
    {   1383.9f,  0.193f },
    {   1760.1f,  0.240f },
    {   1842.8f,  0.171f },
    {   1941.6f,  0.315f },
    {   2339.3f,  0.240f },
    {   2456.9f,  0.419f },
    {   2571.8f,  0.218f },
    {   2707.2f,  0.167f },
    {   2838.2f,  0.357f },
    {   3119.8f,  0.637f },
    {   3288.6f,  0.412f },
    {   3381.8f,  0.379f },
    {   3487.7f,  0.323f },
    {   3660.3f,  1.000f, true },
    {   4041.1f,  0.404f },
    {   4475.4f,  0.196f },
    {   4987.8f,  0.197f },
    {   5239.9f,  0.194f },
    {   5496.6f,  0.386f },
    {   5784.2f,  0.329f },
    {   6087.3f,  0.923f, true },
    {   6416.1f,  0.428f },
    {   6771.3f,  0.593f },
    {   7124.4f,  1.000f, true },
    {   7496.1f,  0.435f },
    {   7870.5f,  0.361f },
    {   8278.6f,  0.330f },
    {   8731.3f,  0.633f },
    {   9751.0f,  0.146f },
    {  10795.0f,  0.192f },
    {  13331.0f,  0.170f },
};
static constexpr int kCymbalPartialCount = 32;

static constexpr HiHatSpec kCymbalSpec = {
    kCymbalPartials,
    kCymbalPartialCount,
    6000.0f,             // noiseHighPassHz    FITTED by resynthesis
    19000.0f,            // noiseLowPassHz     (all four; re-fitted against
    0.110f,              // tonalMix            the clean sample)
    1.300f,              // noiseMix
    0.800f,              // saturationDrive
    1.30f,               // outputTrim
    0.0006f,             // attackTimeConstantSeconds
    0.22f,               // clickAmount — less click than a hat
    0.004f,              // clickDecaySeconds
    0.55f,               // bellAccentAmount — the ting is the cymbal's whole
    0.120f,              // bellAccentDecaySeconds   identity, so it lingers
    0.750f,              // envelopeFastWeight  \ MEASURED off the clean hit by
    0.018f,              // fastDecaySeconds    | a two-exponential fit
    0.180f,              // slowDecaySeconds    / (residual 0.027)
    true,                // decayScalesTimeConstants — the knob shortens the
                         // actual ring, not just the gate
    1.1768f,             // referenceDurationSeconds — MEASURED length after
                         // onset (51898 frames / 44100); -60 dB at 1.00 s
    0.006f,              // minimumDecaySeconds
    0.070f,              // minimumDurationSeconds
    0.650f,              // gateFadeMaxSeconds — long, or the tail clicks off
    0.0011f,             // lineWobbleDepth — more drift than a hat; it is what
                         // stops a long metal tail turning into a chord
    0.026f,              // lineWobbleCorrelationSeconds
};

} // namespace SynthDrums606

#endif /* SD606_CYMBAL_H */
