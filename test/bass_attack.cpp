// Verify audible onset range, exact default bypass, tail preservation and retriggering.
#include "sd606_bass_voice.h"
#include <cmath>
#include <initializer_list>
#include <cstdio>
using namespace SynthDrums606;
int main() {
    int failures = 0;
    for (int rate : {44100, 48000}) {
        double energy[4] = {};
        int k = 0;
        for (int pot : {0, 64, 120, 127}) {
            Sd606BassDrumVoice voice;
            BassDrumVoice reference;
            voice.init(rate); reference.init(rate);
            const float attack = float(pot) / 127.0f;
            for (int hit = 0; hit < 2; ++hit) {
                voice.trigger(attack, 24.0f/127, -2.22f);
                reference.trigger(attack, 24.0f/127, -2.22f);
                for (int i = 0; i < rate / 2; ++i) {
                    float a = voice.process(), b = reference.process();
                    if (!std::isfinite(a)) ++failures;
                    if ((pot >= 120 || i >= rate/50) && a != b) ++failures;
                    if (!hit && i < rate/200) energy[k] += double(a)*a;
                }
            }
            ++k;
        }
        const double contrast = 10*std::log10(energy[2]/energy[0]);
        std::printf("%d Hz: first 5 ms default/minimum contrast %.2f dB\n", rate, contrast);
        if (contrast < 12 || !(energy[0] < energy[1] && energy[1] < energy[2])) ++failures;
    }
    std::printf("Attack: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
