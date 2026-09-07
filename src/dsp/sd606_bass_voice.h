#pragma once

#include "BassDrum.hpp"

namespace SynthDrums606 {

// The upstream transient range changes the click and pitch sweep only subtly.
// Add an onset fade below the fitted Attack default (120/127), giving the
// control a soft-to-punchy range without changing the fitted kick or its tail.
// Keep the vendor voice untouched; at/above the default this is an exact bypass.
class Sd606BassDrumVoice : public BassDrumVoice {
public:
    void init(double sampleRate, uint32_t seed = 0x606606u) {
        sampleRate_ = static_cast<float>(sampleRate);
        remaining_ = 0;
        BassDrumVoice::init(sampleRate, seed);
    }

    void trigger(float transient, float decay, float tune, float jitter = 0.0f) {
        BassDrumVoice::trigger(transient, decay, tune, jitter);
        const float softness = 1.0f - clampf(transient / (120.0f / 127.0f), 0.0f, 1.0f);
        total_ = remaining_ = static_cast<int>(sampleRate_ * 0.020f * softness);
    }

    float process() {
        const float raw = BassDrumVoice::process();
        if (remaining_ <= 0) return raw;
        const float t = static_cast<float>(total_ - remaining_) / total_;
        --remaining_;
        return raw * (t * t * (3.0f - 2.0f * t));
    }

private:
    float sampleRate_ = 44100.0f;
    int remaining_ = 0, total_ = 0;
};

} // namespace SynthDrums606
