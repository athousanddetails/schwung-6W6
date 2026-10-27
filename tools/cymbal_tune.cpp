/*
 * cymbal_tune.cpp — dev tool, not part of the module.
 *
 * The partial table and the envelope of the cymbal are measured. The VOICING
 * fields of a HiHatSpec (how much tonal vs noise, where the noise band sits,
 * how hard it saturates) are not something an FFT hands you, so they get
 * fitted the only way that means anything: render the voice, compare it to the
 * hardware recording, and keep what sounds closest by the numbers.
 *
 * Score = mean absolute error in dB across 1/3-octave bands, plus the error in
 * the decay envelope over time. Both are level-normalised, so this measures
 * SHAPE, not loudness.
 *
 *   ./cymbal_tune <reference.wav> [--grid]
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <complex>
#include <algorithm>

#include "HiHats.hpp"
#include "sd606_cymbal.h"
#include "sd606_metal_hw.h"
using namespace SynthDrums606;

/* ---- reference WAV (24-bit or 16-bit mono) ---- */
static std::vector<float> load_wav(const char *path, int *rate)
{
    std::vector<float> out;
    FILE *f = fopen(path, "rb");
    if(!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    unsigned char hdr[44];
    if(fread(hdr, 1, 44, f) != 44) { fprintf(stderr, "short header\n"); exit(1); }
    /* walk chunks properly rather than trusting a fixed 44-byte header */
    fseek(f, 12, SEEK_SET);
    int bits = 16, ch = 1; long data_len = 0;
    *rate = 44100;
    for(;;)
    {
        unsigned char c[8];
        if(fread(c, 1, 8, f) != 8) break;
        const long sz = c[4] | (c[5]<<8) | (c[6]<<16) | ((long)c[7]<<24);
        if(!memcmp(c, "fmt ", 4))
        {
            unsigned char fmt[16];
            fread(fmt, 1, 16, f);
            ch    = fmt[2] | (fmt[3]<<8);
            *rate = fmt[4] | (fmt[5]<<8) | (fmt[6]<<16) | (fmt[7]<<24);
            bits  = fmt[14] | (fmt[15]<<8);
            fseek(f, sz - 16, SEEK_CUR);
        }
        else if(!memcmp(c, "data", 4)) { data_len = sz; break; }
        else fseek(f, sz + (sz & 1), SEEK_CUR);
    }
    const int bytes = bits / 8;
    const long frames = data_len / (bytes * ch);
    out.reserve((size_t)frames);
    for(long i = 0; i < frames; ++i)
    {
        unsigned char b[4] = {0,0,0,0};
        fread(b, 1, (size_t)(bytes * ch), f);
        int v = 0;
        if(bits == 24) { v = b[0] | (b[1]<<8) | (b[2]<<16); if(v & 0x800000) v -= 0x1000000;
                         out.push_back((float)v / 8388608.0f); }
        else if(bits == 16) { v = (int16_t)(b[0] | (b[1]<<8)); out.push_back((float)v / 32768.0f); }
        else { out.push_back(0.0f); }
    }
    fclose(f);
    return out;
}

/* ---- analysis ---- */
static void fft(std::vector<std::complex<double> > &a)
{
    const size_t n = a.size();
    for(size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for(; j & bit; bit >>= 1) j ^= bit;
        j |= bit;
        if(i < j) std::swap(a[i], a[j]);
    }
    for(size_t len = 2; len <= n; len <<= 1)
    {
        const double ang = -2.0 * M_PI / (double)len;
        const std::complex<double> wl(cos(ang), sin(ang));
        for(size_t i = 0; i < n; i += len)
        {
            std::complex<double> w(1.0, 0.0);
            for(size_t k = 0; k < len/2; ++k)
            {
                const std::complex<double> u = a[i+k], v = a[i+k+len/2] * w;
                a[i+k] = u + v; a[i+k+len/2] = u - v; w *= wl;
            }
        }
    }
}

#define NBANDS 28
static const double kBandLo = 120.0;      /* 1/3-octave from 120 Hz up */

/* Mean spectrum over the whole hit, as 1/3-octave band energies in dB. */
static void band_profile(const std::vector<float> &x_in, int rate, double *out)
{
    const size_t N = 8192;
    /* a 166 ms closed hat is shorter than one window: zero-pad, or the
     * spectrum is empty and the score silently becomes envelope-only */
    std::vector<float> x = x_in; if(x.size() < N + 1) x.resize(N + 1, 0.0f);
    double acc[NBANDS] = {0};
    int windows = 0;
    for(size_t off = 0; off + N < x.size(); off += N/2)
    {
        std::vector<std::complex<double> > buf(N);
        for(size_t i = 0; i < N; ++i)
            buf[i] = x[off+i] * 0.5 * (1 - cos(2*M_PI*(double)i/(double)(N-1)));
        fft(buf);
        for(int b = 0; b < NBANDS; ++b)
        {
            const double lo = kBandLo * pow(2.0, b/3.0);
            const double hi = kBandLo * pow(2.0, (b+1)/3.0);
            const size_t i0 = (size_t)(lo * N / rate), i1 = (size_t)(hi * N / rate);
            double e = 0;
            for(size_t i = i0; i < i1 && i < N/2; ++i) e += std::norm(buf[i]);
            acc[b] += e;
        }
        ++windows;
    }
    double tot = 0;
    for(int b = 0; b < NBANDS; ++b) { acc[b] /= (windows ? windows : 1); tot += acc[b]; }
    for(int b = 0; b < NBANDS; ++b)
        out[b] = 10.0 * log10(acc[b] / (tot > 0 ? tot : 1) + 1e-12);   /* normalised */
}

/* Within-band crest: peak bin over median bin, in dB, per 1/3-octave band.
 * This is what separates 64 resonant lines from filtered noise with the same
 * gross spectrum — the thing that makes a cymbal metal rather than a hiss. */
static void crest_profile(const std::vector<float> &x_in, int rate, double *out)
{
    const size_t N = 8192;
    /* a 166 ms closed hat is shorter than one window: zero-pad, or the
     * spectrum is empty and the score silently becomes envelope-only */
    std::vector<float> x = x_in; if(x.size() < N + 1) x.resize(N + 1, 0.0f);
    double acc[NBANDS] = {0};
    int windows = 0;
    for(size_t off = 0; off + N < x.size(); off += N)
    {
        std::vector<std::complex<double> > buf(N);
        for(size_t i = 0; i < N; ++i)
            buf[i] = x[off+i] * 0.5 * (1 - cos(2*M_PI*(double)i/(double)(N-1)));
        fft(buf);
        for(int b = 0; b < NBANDS; ++b)
        {
            const double lo = kBandLo * pow(2.0, b/3.0);
            const double hi = kBandLo * pow(2.0, (b+1)/3.0);
            const size_t i0 = (size_t)(lo * N / rate), i1 = (size_t)(hi * N / rate);
            std::vector<double> m;
            for(size_t i = i0; i < i1 && i < N/2; ++i) m.push_back(std::abs(buf[i]));
            if(m.size() < 4) continue;
            std::vector<double> srt = m;
            std::sort(srt.begin(), srt.end());
            const double med = srt[srt.size()/2];
            const double mx  = srt.back();
            acc[b] += 20.0 * log10((mx + 1e-12) / (med + 1e-12));
        }
        ++windows;
    }
    for(int b = 0; b < NBANDS; ++b) out[b] = acc[b] / (windows ? windows : 1);
}

#define NENV 40
static void env_profile(const std::vector<float> &x, int rate, double *out)
{
    const size_t hop = x.size() / NENV;
    double peak = 1e-12;
    for(int i = 0; i < NENV; ++i)
    {
        double e = 0; size_t n = 0;
        for(size_t j = i*hop; j < (i+1)*hop && j < x.size(); ++j, ++n) e += x[j]*x[j];
        out[i] = n ? sqrt(e/n) : 0.0;
        if(out[i] > peak) peak = out[i];
    }
    for(int i = 0; i < NENV; ++i) out[i] = 20.0 * log10(out[i]/peak + 1e-6);
    /* clamp at -50 dB: past that the recording is showing its analog noise
     * floor and the synth is at digital zero. Comparing those two says
     * nothing about the model. */
    for(int i = 0; i < NENV; ++i) if(out[i] < -50.0) out[i] = -50.0;
}

static std::vector<float> render(const HiHatSpec &spec, int rate, double seconds)
{
    MetalHiHatVoice v;
    v.init((double)rate, 0x6066u);
    v.trigger(spec, 1.0f, 1.0f);
    std::vector<float> out((size_t)(rate * seconds));
    for(size_t i = 0; i < out.size(); ++i) out[i] = v.process();
    return out;
}

int main(int argc, char **argv)
{
    if(argc < 2) { fprintf(stderr, "usage: cymbal_tune <reference.wav> [--grid]\n"); return 1; }
    int rate = 44100;
    std::vector<float> ref = load_wav(argv[1], &rate);
    /* trim to onset, same as the fitter */
    float pk = 0; for(float s : ref) if(fabsf(s) > pk) pk = fabsf(s);
    size_t on = 0; while(on < ref.size() && fabsf(ref[on]) < pk*0.10f) ++on;
    ref.erase(ref.begin(), ref.begin() + (long)on);
    const double seconds = (double)ref.size() / rate;

    double ref_band[NBANDS], ref_env[NENV], ref_crest[NBANDS];
    band_profile(ref, rate, ref_band);
    env_profile(ref, rate, ref_env);
    crest_profile(ref, rate, ref_crest);

    bool openhat = false, grid = false;
    int cap = 0;
    const char *voice = "cy";
    for(int i = 2; i < argc; ++i)
    {
        if(!strcmp(argv[i], "--grid"))    grid = true;
        if(!strcmp(argv[i], "--openhat")) openhat = true;          /* vendored OH spec: the yardstick */
        if(!strcmp(argv[i], "--partials") && i+1 < argc) cap = atoi(argv[++i]);
        if(!strcmp(argv[i], "--voice") && i+1 < argc) voice = argv[++i];
    }
    HiHatSpec base = openhat ? kOpenHatSpec
                   : !strcmp(voice, "oh") ? kHwOpenHatSpec
                   : !strcmp(voice, "ch") ? kHwClosedHatSpec
                   : kCymbalSpec;

    /* Partial count is the CPU lever: every line is one sin() per sample. Keep
     * the N LOUDEST (still in frequency order) and score what that costs. */
    static Partial trimmed[64];
    if(cap > 0 && cap < base.partialCount)
    {
        std::vector<Partial> v(base.partials, base.partials + base.partialCount);
        std::vector<Partial> byamp = v;
        std::sort(byamp.begin(), byamp.end(),
                  [](const Partial &a, const Partial &b){ return a.amplitude > b.amplitude; });
        const float cut = byamp[(size_t)cap - 1].amplitude;
        int n = 0;
        for(const Partial &p : v) if(p.amplitude >= cut && n < cap) trimmed[n++] = p;
        base.partials = trimmed;
        base.partialCount = n;
        printf("partial cap: %d -> %d lines\n", (int)v.size(), n);
    }

    struct Best { double score, band, env, crest; HiHatSpec spec; } best;
    best.score = 1e9;

    /* Refinement pass. The first grid put the optimum on the noiseMix and
     * saturation ceilings, so both are extended past where it landed — an
     * optimum sitting on a boundary is a search artefact until proven
     * otherwise. */
    /* Full range again: the metric changed, so the previous optimum tells us
     * nothing. Every axis extends past where the old search landed. */
    const float tonal[]  = { 0.06f, 0.11f, 0.18f, 0.28f, 0.42f, 0.60f };
    const float noise[]  = { 0.50f, 0.70f, 0.91f, 1.10f, 1.30f, 1.50f };
    const float hp[]     = { 3400.f, 4200.f, 5000.f, 6000.f, 6800.f, 7600.f };
    const float lp[]     = { 12500.f, 16000.f, 19000.f };
    const float sat[]    = { 0.30f, 0.45f, 0.60f, 0.80f, 1.00f };

    int tried = 0;
    if(grid) for(float t : tonal) for(float nz : noise) for(float h : hp)
    for(float l : lp)    for(float sd : sat)
    {
        if(!grid && !(t==base.tonalMix && nz==base.noiseMix)) { /* still scan */ }
        HiHatSpec s = base;
        s.tonalMix = t; s.noiseMix = nz;
        s.noiseHighPassHz = h; s.noiseLowPassHz = l; s.saturationDrive = sd;

        std::vector<float> got = render(s, rate, seconds);
        double gb[NBANDS], ge[NENV], gc[NBANDS];
        band_profile(got, rate, gb);
        env_profile(got, rate, ge);
        crest_profile(got, rate, gc);

        double be = 0; for(int b = 0; b < NBANDS; ++b) be += fabs(gb[b] - ref_band[b]);
        be /= NBANDS;
        double ee = 0; for(int i = 0; i < NENV; ++i) ee += fabs(ge[i] - ref_env[i]);
        ee /= NENV;
        double ce = 0; for(int b = 0; b < NBANDS; ++b) ce += fabs(gc[b] - ref_crest[b]);
        ce /= NBANDS;
        const double score = be + 0.5 * ee + ce;
        if(score < best.score) { best.score = score; best.band = be; best.env = ee;
                                 best.crest = ce; best.spec = s; }
        ++tried;
    }

    /* how close is the vendored OPEN HAT spec to a real open hat? that is the
     * yardstick for "good", since it is a shipped, accepted fit. */
    printf("reference: %s  (%.3f s after onset, %d Hz)\n", argv[1], seconds, rate);
    printf("searched %d voicings\n\n", tried);
    if(grid) printf("BEST: tonalMix %.2f  noiseMix %.2f  noiseHP %.0f  noiseLP %.0f  sat %.2f\n",
           best.spec.tonalMix, best.spec.noiseMix, best.spec.noiseHighPassHz,
           best.spec.noiseLowPassHz, best.spec.saturationDrive);
    if(grid) printf("      spectral %.2f dB/band, envelope %.2f dB, crest %.2f dB, score %.2f\n",
           best.band, best.env, best.crest, best.score);
    if(grid)
    {
        /* The metric cannot hear "the ting pokes out"; an owner can. Offer the
         * best-by-metric and two progressively more tonal neighbours. */
        printf("CANDIDATES (tonalMix noiseMix noiseHP noiseLP sat):\n");
        printf("  A metric-best : %.3f %.3f %.0f %.0f %.2f\n", best.spec.tonalMix, best.spec.noiseMix,
               best.spec.noiseHighPassHz, best.spec.noiseLowPassHz, best.spec.saturationDrive);
        printf("  B more tonal  : %.3f %.3f %.0f %.0f %.2f\n", best.spec.tonalMix*1.8f, best.spec.noiseMix*0.85f,
               best.spec.noiseHighPassHz, best.spec.noiseLowPassHz, best.spec.saturationDrive);
        printf("  C most tonal  : %.3f %.3f %.0f %.0f %.2f\n", best.spec.tonalMix*3.0f, best.spec.noiseMix*0.7f,
               best.spec.noiseHighPassHz, best.spec.noiseLowPassHz, best.spec.saturationDrive);
    }

    HiHatSpec cur = base;
    std::vector<float> got = render(cur, rate, seconds);
    double gb[NBANDS], ge[NENV], gc[NBANDS];
    band_profile(got, rate, gb); env_profile(got, rate, ge); crest_profile(got, rate, gc);
    double be = 0; for(int b = 0; b < NBANDS; ++b) be += fabs(gb[b] - ref_band[b]); be /= NBANDS;
    double ee = 0; for(int i = 0; i < NENV; ++i) ee += fabs(ge[i] - ref_env[i]); ee /= NENV;
    double ce = 0; for(int b = 0; b < NBANDS; ++b) ce += fabs(gc[b] - ref_crest[b]); ce /= NBANDS;
    printf("\nCURRENT %s voicing: spectral %.2f dB/band, envelope %.2f dB, crest %.2f dB, score %.2f\n",
           openhat ? "vendored kOpenHatSpec" : "sd606_cymbal.h", be, ee, ce,
           be + 0.5*ee + ce);
    return 0;
}
