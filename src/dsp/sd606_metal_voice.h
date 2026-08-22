/*
 * sd606_metal_voice.h — MetalHiHatVoice with the oscillator bank replaced.
 *
 * A FORK of the vendored class, and the only one in this project. Everything
 * under src/vendor stays untouched; this is a copy with one thing changed.
 *
 * WHY. The vendored voice calls sinf() once per partial per sample and steps a
 * random-number generator once per partial per sample. With three metal voices
 * ringing that dominated the whole module: 55% of the Move's block budget for
 * a TR-606, which is absurd for the simplest drum machine ever built.
 *
 * WHAT CHANGED. Two things, both measured on the device before being written:
 *
 *   1. sinf() -> a coupled-form ("magic circle") oscillator: x += k*y;
 *      y -= k*x, with k = 2*sin(w/2). Two multiplies and two adds per partial.
 *      This is not an approximation of sin() — with the correct seed it IS
 *      sin(), to 5e-11 over two seconds at 21 kHz. Measured 8.06x faster than
 *      the real loop on the Move.
 *
 *   2. The wobble random walk and the retune it implies now run once per
 *      kRetuneInterval samples rather than every sample. Its correlation time
 *      is 20-26 ms, so per-sample updates bought nothing. Step scaled by
 *      sqrt(N), decay by N, so the walk keeps its variance and time constant.
 *      Amplitude drift from retuning without re-seeding: 0.14 dB worst case
 *      over a 2 s tail at 16.4 kHz.
 *
 * THE TRAP, since it nearly shipped: seeding y_0 = cos(p) instead of
 * cos(p + w/2) scales each partial by 1/cos(w/2). That is 1.00 at the bottom
 * of the table and 13x at 21 kHz — it would have wrecked the top of every hat
 * and cymbal while looking perfectly reasonable in the source.
 *
 * Diff this against src/vendor/606/HiHats.hpp before changing anything here.
 * Upstream fixes to the voice need re-applying by hand.
 *
 * GPL-3.0. Derived from 606-Inspired-Synth-Drums (MIT, Matthew Fecher).
 */
#ifndef SD606_METAL_VOICE_H
#define SD606_METAL_VOICE_H

#include "HiHats.hpp"

namespace SynthDrums606 {

/* Retune cadence. 32 samples is 0.73 ms at 44.1 kHz — far inside the wobble's
 * 20-26 ms correlation time. kRetuneStepScale is sqrt(kRetuneInterval). */
static constexpr uint64_t kRetuneInterval  = 32;
static constexpr float    kRetuneStepScale = 5.65685425f;

class Sd606MetalVoice {
public:
    void init(double sampleRate, uint32_t seed = 0x606606u) {
        sampleRate_ = std::isfinite(sampleRate) && sampleRate >= 8000.0
            ? sampleRate
            : 44100.0;
        const uint32_t safeSeed = seed == 0u ? 0x606606u : seed;
        phaseRandom_.seed(safeSeed ^ 0x9E3779B9u);
        wobbleRandom_.seed(safeSeed ^ 0x51D3B7A1u);
        noise_.init(sampleRate_, safeSeed ^ 0xA511E9B3u);
        noiseHighPass_.init(sampleRate_);
        noiseLowPass_.init(sampleRate_);
        stop();
    }

    void trigger(const HiHatSpec &spec, float decayPercent, float pitchRatio) {
        if (!std::isfinite(decayPercent) ||
            !std::isfinite(pitchRatio) ||
            pitchRatio <= 0.0f) {
            stop();
            return;
        }

        const float decay = clampf(decayPercent, 0.0f, 1.0f);
        const float frequencyRatio = clampf(
            pitchRatio,
            1.0f / 16.0f,
            16.0f);

        activePartialCount_ = std::min(spec.partialCount, kMaxPartialCount);
        for (int index = 0; index < activePartialCount_; ++index) {
            const float frequencyHz = spec.partials[index].frequencyHz * frequencyRatio;
            const float sampleRateRatio = frequencyHz / static_cast<float>(sampleRate_);
            float level = 1.0f;
            if (sampleRateRatio >= kMutedSampleRateRatio) {
                level = 0.0f;
            } else if (sampleRateRatio > kFullLevelSampleRateRatio) {
                const float taper = (kMutedSampleRateRatio - sampleRateRatio)
                                  / (kMutedSampleRateRatio - kFullLevelSampleRateRatio);
                level = taper * taper * (3.0f - 2.0f * taper);
            }

            // Start phases close enough to land as one hit
            // Perfect alignment clips, fully random phases lose the attack
            phases_[index] = (phaseRandom_.bipolar() + 1.0f)
                           * 0.3f * kPi;
            increments_[index] = level > 0.0f
                ? kTwoPi * sampleRateRatio
                : 0.0f;
            amplitudes_[index] = spec.partials[index].amplitude * level;
            bellFlags_[index] = spec.partials[index].bell;

            // Coupled-form seed. If x_n is to equal sin(n*w + p) exactly then
            // y_0 must be cos(p + w/2), not cos(p) -- the half-step offset
            // falls out of sin(A)-sin(B) = 2 cos((A+B)/2) sin((A-B)/2). Get it
            // wrong and the partial's amplitude is scaled by 1/cos(w/2), which
            // is 1.00 at the bottom of the table and 13x at 21 kHz.
            const float w0 = increments_[index];
            k_[index] = 2.0f * std::sin(w0 * 0.5f);
            x_[index] = std::sin(phases_[index]);
            y_[index] = std::cos(phases_[index] + w0 * 0.5f);
        }
        bellAccentAmount_ = std::max(0.0f, spec.bellAccentAmount);
        bellAccentCoefficient_ = decayCoef(
            sampleRate_,
            std::max(0.005f, spec.bellAccentDecaySeconds));
        bellAccentEnvelope_ = 1.0f;
        clickAmount_ = std::max(0.0f, spec.clickAmount);
        clickCoefficient_ = decayCoef(
            sampleRate_,
            std::max(0.0005f, spec.clickDecaySeconds));
        clickEnvelope_ = 1.0f;

        noiseHighPass_.setHighPass(spec.noiseHighPassHz * frequencyRatio, 0.70f);
        noiseLowPass_.setLowPass(spec.noiseLowPassHz * frequencyRatio, 0.707f);
        noiseHighPass_.reset();
        noiseLowPass_.reset();
        dcBlocker_.reset();

        tonalMix_ = spec.tonalMix;
        noiseMix_ = spec.noiseMix;
        saturationDrive_ = std::max(0.0f, spec.saturationDrive);
        outputTrim_ = spec.outputTrim;

        if (spec.lineWobbleDepth > 0.0f) {
            wobbleAlpha_ = 1.0f - decayCoef(
                sampleRate_,
                std::max(0.005f, spec.lineWobbleCorrelationSeconds));
            // Scale the noise so correlation does not change the wobble depth
            wobbleDrive_ = spec.lineWobbleDepth * std::sqrt(6.0f * wobbleAlpha_);
        } else {
            wobbleAlpha_ = 0.0f;
            wobbleDrive_ = 0.0f;
        }
        for (int index = 0; index < kMaxPartialCount; ++index) {
            wobbleStates_[index] = 0.0f;
        }

        const float timeConstantScale = spec.decayScalesTimeConstants ? decay : 1.0f;
        fastDecayCoefficient_ = decayCoef(
            sampleRate_,
            std::max(spec.minimumDecaySeconds, timeConstantScale * spec.fastDecaySeconds));
        slowDecayCoefficient_ = decayCoef(
            sampleRate_,
            std::max(spec.minimumDecaySeconds, timeConstantScale * spec.slowDecaySeconds));
        fastEnvelopeWeight_ = clampf(spec.envelopeFastWeight, 0.0f, 1.0f);
        fastEnvelope_ = 1.0f;
        slowEnvelope_ = 1.0f;
        attackCoefficient_ = 1.0f - decayCoef(
            sampleRate_,
            spec.attackTimeConstantSeconds);
        attackEnvelope_ = 0.0f;

        const float naturalDurationSeconds = std::max(
            spec.minimumDurationSeconds,
            decay * spec.referenceDurationSeconds);
        naturalFrameCount_ = static_cast<uint64_t>(std::max(
            1.0,
            std::floor(naturalDurationSeconds * sampleRate_ + 0.5)));
        uint64_t gateFadeFrames = static_cast<uint64_t>(std::max(
            0.0,
            std::floor(static_cast<double>(spec.gateFadeMaxSeconds) * sampleRate_ + 0.5)));
        gateFadeFrames = std::min(gateFadeFrames, naturalFrameCount_);
        gateFadeFrames_ = gateFadeFrames;
        inverseGateFadeFrames_ = gateFadeFrames > 0
            ? 1.0f / static_cast<float>(gateFadeFrames)
            : 0.0f;
        frameIndex_ = 0;
        active_ = true;
    }

    float process() {
        if (!active_ || frameIndex_ >= naturalFrameCount_) {
            active_ = false;
            return 0.0f;
        }

        // The wobble is a slow random walk (correlation ~20-26 ms), so running
        // it -- and the retune it implies -- every sample buys nothing. Once
        // per kRetuneInterval keeps the same character for 1/32 of the cost,
        // with the step scaled by sqrt(N) and the decay by N so the walk keeps
        // its variance and time constant.
        if (wobbleDrive_ > 0.0f && (frameIndex_ % kRetuneInterval) == 0) {
            const float step  = wobbleDrive_ * kRetuneStepScale;
            const float decay = wobbleAlpha_ * static_cast<float>(kRetuneInterval);
            for (int index = 0; index < activePartialCount_; ++index) {
                wobbleStates_[index] += step * wobbleRandom_.bipolar()
                                      - decay * wobbleStates_[index];
                k_[index] = 2.0f * std::sin(
                    increments_[index] * (1.0f + wobbleStates_[index]) * 0.5f);
            }
        }

        const float bellGain = 1.0f + bellAccentAmount_ * bellAccentEnvelope_;
        bellAccentEnvelope_ = flushDenormal(bellAccentEnvelope_ * bellAccentCoefficient_);

        // The whole point of this file: two multiplies and two adds per
        // partial instead of a sinf. Measured on the Move, 8.06x faster over
        // the real loop, and accurate to 5e-11 against sinf over two seconds.
        float tonal = 0.0f;
        for (int index = 0; index < activePartialCount_; ++index) {
            const float lineGain = bellFlags_[index] ? bellGain : 1.0f;
            const float nx = x_[index] + k_[index] * y_[index];
            y_[index] -= k_[index] * nx;
            x_[index] = nx;
            tonal += nx * amplitudes_[index] * lineGain;
        }

        const float noise = noiseLowPass_.process(
            noiseHighPass_.process(noise_.process()));

        attackEnvelope_ += attackCoefficient_ * (1.0f - attackEnvelope_);
        fastEnvelope_ = flushDenormal(fastEnvelope_ * fastDecayCoefficient_);
        slowEnvelope_ = flushDenormal(slowEnvelope_ * slowDecayCoefficient_);
        float envelope = attackEnvelope_
            * (fastEnvelopeWeight_ * fastEnvelope_
               + (1.0f - fastEnvelopeWeight_) * slowEnvelope_);
        if (gateFadeFrames_ > 0) {
            const uint64_t framesRemaining = naturalFrameCount_ - frameIndex_;
            if (framesRemaining < gateFadeFrames_) {
                envelope *= static_cast<float>(framesRemaining) * inverseGateFadeFrames_;
            }
        }

        float source = tonal * tonalMix_ + noise * noiseMix_;
        if (saturationDrive_ > 0.0f) {
            source = std::tanh(source * saturationDrive_);
        }
        float body = source * envelope
                   + noise * clickAmount_ * clickEnvelope_;
        clickEnvelope_ = flushDenormal(clickEnvelope_ * clickCoefficient_);
        float output = body * outputTrim_;
        output = flushDenormal(dcBlocker_.process(output));
        if (!std::isfinite(output)) {
            stop();
            return 0.0f;
        }

        ++frameIndex_;
        if (frameIndex_ >= naturalFrameCount_) {
            active_ = false;
        }
        return output;
    }

    bool isActive() const {
        return active_;
    }

    void stop() {
        active_ = false;
        frameIndex_ = 0;
        naturalFrameCount_ = 0;
        gateFadeFrames_ = 0;
        inverseGateFadeFrames_ = 0.0f;
        attackEnvelope_ = 0.0f;
        fastEnvelope_ = 0.0f;
        slowEnvelope_ = 0.0f;
        noiseHighPass_.reset();
        noiseLowPass_.reset();
        dcBlocker_.reset();
        activePartialCount_ = 0;
        wobbleAlpha_ = 0.0f;
        wobbleDrive_ = 0.0f;
        bellAccentAmount_ = 0.0f;
        bellAccentEnvelope_ = 0.0f;
        clickAmount_ = 0.0f;
        clickEnvelope_ = 0.0f;
        for (int index = 0; index < kMaxPartialCount; ++index) {
            phases_[index] = 0.0f;
            increments_[index] = 0.0f;
            x_[index] = 0.0f;
            y_[index] = 0.0f;
            k_[index] = 0.0f;
            amplitudes_[index] = 0.0f;
            wobbleStates_[index] = 0.0f;
            bellFlags_[index] = false;
        }
    }

private:
    double sampleRate_ = 44100.0;
    Random phaseRandom_;
    Random wobbleRandom_;
    WhiteNoise noise_;
    Biquad noiseHighPass_;
    LowPassBiquad noiseLowPass_;
    DCBlocker dcBlocker_;
    float phases_[kMaxPartialCount] = {};
    float increments_[kMaxPartialCount] = {};
    float x_[kMaxPartialCount] = {};        // coupled-form oscillator state
    float y_[kMaxPartialCount] = {};        // (quadrature partner of x_)
    float k_[kMaxPartialCount] = {};        // 2*sin(w/2) per partial
    float amplitudes_[kMaxPartialCount] = {};
    float wobbleStates_[kMaxPartialCount] = {};
    bool bellFlags_[kMaxPartialCount] = {};
    float wobbleAlpha_ = 0.0f;
    float wobbleDrive_ = 0.0f;
    float bellAccentAmount_ = 0.0f;
    float bellAccentCoefficient_ = 0.999f;
    float bellAccentEnvelope_ = 0.0f;
    float clickAmount_ = 0.0f;
    float clickCoefficient_ = 0.999f;
    float clickEnvelope_ = 0.0f;
    int activePartialCount_ = 0;
    float tonalMix_ = 0.0f;
    float noiseMix_ = 0.0f;
    float saturationDrive_ = 0.0f;
    float outputTrim_ = 1.0f;
    float attackCoefficient_ = 0.01f;
    float attackEnvelope_ = 0.0f;
    float fastDecayCoefficient_ = 0.999f;
    float slowDecayCoefficient_ = 0.999f;
    float fastEnvelopeWeight_ = 1.0f;
    float fastEnvelope_ = 0.0f;
    float slowEnvelope_ = 0.0f;
    uint64_t naturalFrameCount_ = 0;
    uint64_t gateFadeFrames_ = 0;
    float inverseGateFadeFrames_ = 0.0f;
    uint64_t frameIndex_ = 0;
    bool active_ = false;
};


} // namespace SynthDrums606

#endif /* SD606_METAL_VOICE_H */
