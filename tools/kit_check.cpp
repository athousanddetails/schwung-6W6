/*
 * kit_check.cpp — score every 6W6 voice, at its DEFAULT pots, against the
 * matching hardware TR-606 recording. Also writes a kit A/B WAV: for each
 * voice, the hardware hit, a gap, ours, a gap.
 *
 *   ./kit_check <dir with "<Name> 606 Clean.wav" files> [ab.wav]
 *
 * Renders through the real engine (sd606_engine), so what is scored is what
 * ships — voice trim, drive stage at unity, the lot — not the bare DSP class.
 * Un-accented hits (velocity 90), because the references are the clean takes.
 */
#include "score.h"
#include "sd606_engine.h"
#include "HiHats.hpp"

struct Ref { const char *file; int voice; const char *label; };

int main(int argc, char **argv)
{
    if(argc < 2) { fprintf(stderr, "usage: kit_check <dir> [ab.wav]\n"); return 1; }
    const Ref refs[] = {
        { "BD 606 Clean.wav",     SD606_BD, "Bass Drum"  },
        { "Snare 606 Clean.wav",  SD606_SD, "Snare"      },
        { "Tom Lo Clean 606.wav", SD606_LT, "Low Tom"    },
        { "Tom Hi Clean 606.wav", SD606_HT, "Hi Tom"     },
        { "CH 606 Clean.wav",     SD606_CH, "Closed Hat" },
        { "OH 606 Clean.wav",     SD606_OH, "Open Hat"   },
        { "Cymbal 606 Clean.wav", SD606_CY, "Cymbal"     },
    };
    std::vector<float> ab;
    int rate = 44100;

    printf("%-11s %9s %9s %9s %7s   %s\n", "voice", "spectral", "envelope", "crest", "score", "ref length");
    printf("%-11s %9s %9s %9s %7s\n", "", "dB/band", "dB", "dB", "");
    double sum = 0; int n = 0;
    for(const Ref &r : refs)
    {
        char path[1024]; snprintf(path, sizeof(path), "%s/%s", argv[1], r.file);
        std::vector<float> ref = load_wav(path, &rate);
        trim_onset(ref);

        sd606_engine_t *e = sd606_create((float)rate);
        sd606_set_param(e, "volume", "127");
        sd606_set_param(e, "master_dist", "0");
        sd606_trigger(e, r.voice, 90);
        std::vector<float> got(ref.size());
        float blk[128];
        for(size_t i = 0; i < got.size(); i += 128)
        {
            sd606_render(e, blk, 128);
            for(int k = 0; k < 128 && i + k < got.size(); ++k) got[i+k] = blk[k];
        }
        sd606_destroy(e);
        trim_onset(got);
        got.resize(ref.size(), 0.0f);

        Profile pr, pg;
        profile(ref, rate, pr, ref.size());
        profile(got, rate, pg, ref.size());
        const Score s = score(pr, pg);
        printf("%-11s %9.2f %9.2f %9.2f %7.2f   %.3f s\n",
               r.label, s.spectral, s.envelope, s.crest, s.total(), (double)ref.size()/rate);
        sum += s.total(); ++n;

        if(argc > 2)
        {
            float rp = 0, gp = 0;
            for(float v : ref) rp = std::max(rp, fabsf(v));
            for(float v : got) gp = std::max(gp, fabsf(v));
            const size_t gap = (size_t)(0.35 * rate);
            for(float v : ref) ab.push_back(v / (rp > 0 ? rp : 1) * 0.8f);
            ab.insert(ab.end(), gap, 0.0f);
            for(float v : got) ab.push_back(v / (gp > 0 ? gp : 1) * 0.8f);
            ab.insert(ab.end(), gap * 2, 0.0f);
        }
    }
    /* Yardstick, computed the same way: Fecher's SHIPPED open-hat spec,
     * through the vendored voice, against the real open hat. Anything at or
     * under this is as good as what already exists and is accepted. */
    double yard = 0;
    {
        char path[1024]; snprintf(path, sizeof(path), "%s/OH 606 Clean.wav", argv[1]);
        std::vector<float> ref = load_wav(path, &rate); trim_onset(ref);
        SynthDrums606::MetalHiHatVoice v; v.init((double)rate, 0x6065u);
        v.trigger(SynthDrums606::kOpenHatSpec, 1.0f, 1.0f);
        std::vector<float> got(ref.size()); for(float &x : got) x = v.process();
        trim_onset(got); got.resize(ref.size(), 0.0f);
        Profile pr, pg; profile(ref, rate, pr, ref.size()); profile(got, rate, pg, ref.size());
        yard = score(pr, pg).total();
    }
    printf("%-11s %39s %7.2f   (yardstick: Fecher's shipped open hat vs the real one = %.2f)\n",
           "mean", "", sum / n, yard);

    if(argc > 2)
    {
        FILE *f = fopen(argv[2], "wb");
        const uint32_t nb = (uint32_t)ab.size() * 2;
        auto p32 = [&](uint32_t v){ fputc(v&255,f); fputc((v>>8)&255,f); fputc((v>>16)&255,f); fputc((v>>24)&255,f); };
        auto p16 = [&](uint32_t v){ fputc(v&255,f); fputc((v>>8)&255,f); };
        fwrite("RIFF",1,4,f); p32(36+nb); fwrite("WAVEfmt ",1,8,f); p32(16); p16(1); p16(1);
        p32((uint32_t)rate); p32((uint32_t)rate*2); p16(2); p16(16); fwrite("data",1,4,f); p32(nb);
        for(float v : ab) { int s = (int)(std::max(-1.0f, std::min(1.0f, v)) * 32767); p16((uint16_t)(int16_t)s); }
        fclose(f);
        printf("A/B written: %s (%.1f s)\n", argv[2], (double)ab.size()/rate);
    }
    return 0;
}
