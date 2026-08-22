/*
 * sd606_metal_hw.h — the hi-hat metal bank fitted from Gustavo's own 606.
 *
 * The vendored hat table is a measurement of ONE TR-606 (Fecher's). This is a
 * measurement of ANOTHER (the unit the Clean Kit samples came from). They
 * differ audibly: that unit's dominant line sits at 7.2-7.3 kHz where the
 * vendored table's sits at 6340. Both are real 606s; this one is the one the
 * owner of this module can hold up against the speaker.
 *
 * One table serves CH and OH because on the hardware they ARE one circuit —
 * six oscillators, two envelopes. The cymbal shares the bank too, but has its
 * own fitted table (sd606_cymbal.h) because its recording reaches down to
 * 266 Hz where a hat's starts at 3.7 kHz.
 *
 * 32 lines, not 47: partial count is the dominant CPU cost of this engine and
 * a measured sweep against these same recordings found no loss down to 24
 * (tools/cymbal_tune.cpp --partials N). 32 leaves margin.
 *
 * Fitted by tools/fit_partials.py from "OH 606 Clean.wav" (same method as
 * the cymbal; the method itself was validated against the vendored table,
 * recovering Fecher's three named lines). Envelope fields are measured by a
 * two-exponential fit of the same recordings. Voicing fields are fitted by
 * resynthesis (tools/cymbal_tune.cpp --voice oh|ch).
 *
 * DECAY CONVENTION: the measured time constants and durations are divided by
 * 0.8, and the decay pots default to 0.8 (pot 102). MetalHiHatVoice scales
 * its time constants by the decay argument, so that renders IDENTICALLY to
 * (measured, 1.0) — and leaves the top fifth of the pot for 'longer than the
 * hardware'. Defaulting a pot to its end stop is bad UX. Same rule in
 * sd606_cymbal.h.
 *
 * GPL-3.0.
 */
#ifndef SD606_METAL_HW_H
#define SD606_METAL_HW_H

#include "HiHats.hpp"

namespace SynthDrums606 {

static constexpr Partial kHwHatPartials[] = {
    {   3721.7f,  0.159f },
    {   3909.9f,  0.119f },
    {   4131.1f,  0.082f },
    {   4556.3f,  0.125f },
    {   4792.8f,  0.170f },
    {   5049.2f,  0.158f },
    {   5317.7f,  0.148f },
    {   5600.9f,  0.368f },
    {   5753.8f,  0.212f },
    {   5909.4f,  0.284f },
    {   6221.9f,  0.899f, true },
    {   6550.3f,  0.365f },
    {   6905.5f,  0.904f, true },
    {   7307.3f,  1.000f, true },
    {   7702.9f,  0.450f },
    {   8108.4f,  0.393f },
    {   8545.3f,  0.705f },
    {   9003.3f,  0.332f },
    {   9472.3f,  0.174f },
    {   9720.6f,  0.123f },
    {   9980.9f,  0.154f },
    {  10537.4f,  0.230f },
    {  11109.7f,  0.299f },
    {  11707.5f,  0.187f },
    {  12332.2f,  0.183f },
    {  12964.5f,  0.193f },
    {  13310.6f,  0.084f },
    {  13676.2f,  0.159f },
    {  14041.5f,  0.122f },
    {  14424.2f,  0.152f },
    {  15184.8f,  0.086f },
    {  16024.9f,  0.104f },
};
static constexpr int kHwHatPartialCount = 32;

/* Open hat: vendored spec with this unit's table, measured envelope and
 * duration. Two-exponential fit of the recording: fast 0.003 s, slow 0.160 s,
 * fast weight ~0 — a single slow decay, -60 dB at 1.043 s. */
static constexpr HiHatSpec kHwOpenHatSpec = {
    kHwHatPartials, kHwHatPartialCount,
    6000.0f,             // noiseHighPassHz   .
    19000.0f,            // noiseLowPassHz     | voicing FITTED by resynthesis
    0.180f,              // tonalMix           | against "OH 606 Clean.wav"
    0.500f,              // noiseMix           | (score 9.07 -> 6.85); the owner
    0.45f,               // saturationDrive    / picked this one by ear
    1.63f,               // outputTrim
    0.0003f, 0.35f, 0.003f, 0.70f, 0.040f,
    0.05f,               // envelopeFastWeight   MEASURED (fit gave 0.00; a
                         //                      little snap keeps the strike)
    0.0125f,             // fastDecaySeconds     \_ MEASURED 0.010 / 0.160 s,
    0.200f,              // slowDecaySeconds     /  divided by 0.8 (see below)
    true,
    1.5085f,             // referenceDurationSeconds  MEASURED 1.2068 s / 0.8
    0.004f, 0.050f, 0.500f, 0.0007f, 0.020f,
};

/* Closed hat: same bank, the fast envelope. Fit: fast 0.009 s, slow 0.050 s,
 * weight 0.85; -60 dB at 0.155 s. */
static constexpr HiHatSpec kHwClosedHatSpec = {
    kHwHatPartials, kHwHatPartialCount,
    7600.0f,             // noiseHighPassHz   .
    16000.0f,            // noiseLowPassHz     | voicing from the same search
    0.324f,              // tonalMix           | against "CH 606 Clean.wav" --
    0.595f,              // noiseMix           | the owner picked the MORE-METAL
    0.80f,               // saturationDrive    / neighbour over the metric-best
    1.75f,               // outputTrim
    0.0003f, 0.35f, 0.003f, 0.70f, 0.040f,
    0.85f,               // envelopeFastWeight   MEASURED
    0.01125f,            // fastDecaySeconds     \_ MEASURED 0.009 / 0.050 s,
    0.0625f,             // slowDecaySeconds     /  divided by 0.8
    true,
    0.2080f,             // referenceDurationSeconds  MEASURED 0.1664 s / 0.8
    0.004f, 0.028f, 0.045f, 0.0007f, 0.020f,
};

} // namespace SynthDrums606

#endif /* SD606_METAL_HW_H */
