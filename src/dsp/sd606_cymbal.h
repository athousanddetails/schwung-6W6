/*
 * sd606_cymbal.h — the voice the upstream repo does not ship.
 *
 * The real TR-606's roster is BD, SD, Low Tom, Hi Tom, CYMBAL, Open HH,
 * Closed HH. 606-Inspired-Synth-Drums covers six of those and adds an RD-6
 * clap, but there is no cymbal in it. This supplies one.
 *
 * On the hardware the cymbal and the hi-hats are the SAME metal source — one
 * bank of six square oscillators, split by different envelopes and filters —
 * so the honest way to build a cymbal here is a third HiHatSpec, not a new
 * engine. MetalHiHatVoice takes any spec; nothing in the vendored code
 * changes.
 *
 * >>> PROVISIONAL SPEC <<<
 * The partial TABLE below is the vendored open-hat table, unchanged. Only the
 * envelope, filter and mix fields are cymbal values. That is deliberate: the
 * hat partials are FFT peaks measured off hardware, and inventing extra
 * frequencies to sit beside them would be guessing dressed up as measurement.
 * A real fit — FFT a hardware 606 cymbal, keep the peaks that survive the
 * decay, same method Fecher used — replaces kCymbalPartials via
 * tools/fit_partials.py. Until then this reads as a long, dark, dense hat,
 * which is a fair sketch of a 606 cymbal but is NOT a measured one.
 *
 * GPL-3.0.
 */
#ifndef SD606_CYMBAL_H
#define SD606_CYMBAL_H

#include "HiHats.hpp"

namespace SynthDrums606 {

/* Replaced by a measured table once a hardware cymbal sample is fitted. */
static constexpr const Partial *kCymbalPartials = kOpenHatPartials;
static constexpr int kCymbalPartialCount = 47;

static constexpr HiHatSpec kCymbalSpec = {
    kCymbalPartials,
    kCymbalPartialCount,
    3200.0f,             // noiseHighPassHz — lower than the hats; a cymbal
                         // keeps low-mid body the hats throw away
    16000.0f,            // noiseLowPassHz
    0.420f,              // tonalMix — far more metal than a hat (0.11) so the
                         // partials ring out instead of hissing
    0.620f,              // noiseMix
    0.45f,               // saturationDrive — gentler; the long tail should not
                         // compress into noise
    1.30f,               // outputTrim
    0.0006f,             // attackTimeConstantSeconds — slightly softer strike
    0.22f,               // clickAmount — less click than a hat
    0.004f,              // clickDecaySeconds
    0.55f,               // bellAccentAmount — the ting is the cymbal's whole
    0.120f,              // bellAccentDecaySeconds   identity, so it lingers
    0.08f,               // envelopeFastWeight — almost all tail, little snap
    0.014f,              // fastDecaySeconds
    1.450f,              // slowDecaySeconds — the long wash
    true,                // decayScalesTimeConstants — the knob shortens the
                         // actual ring, not just the gate
    2.10f,               // referenceDurationSeconds — a 606 cymbal rings well
                         // past the open hat's 1.77 s
    0.006f,              // minimumDecaySeconds
    0.070f,              // minimumDurationSeconds
    0.650f,              // gateFadeMaxSeconds — long fade or the tail clicks
    0.0011f,             // lineWobbleDepth — more drift than a hat; it is what
                         // stops a 2 s metal tail turning into a chord
    0.026f,              // lineWobbleCorrelationSeconds
};

} // namespace SynthDrums606

#endif /* SD606_CYMBAL_H */
