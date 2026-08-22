/*
 * score.h — how a rendered voice is compared to a hardware recording.
 *
 * Three measures, all level-normalised so they see SHAPE, not loudness:
 *   spectral — mean |dB error| across 1/3-octave bands, averaged over the hit
 *   envelope — mean |dB error| of the RMS decay curve in fixed 10 ms frames,
 *              over the reference's own length, clamped at -50 dB (below that
 *              the recording is showing its analog floor, the synth digital
 *              zero, and the difference says nothing about the model)
 *   crest    — mean |dB error| of within-band peak/median, which is what tells
 *              64 resonant lines from filtered noise of the same gross shape
 *
 * Shared by cymbal_tune (one voice, grid search) and kit_check (every voice,
 * at defaults). Dev tools only; nothing here ships.
 */
#ifndef SD606_SCORE_H
#define SD606_SCORE_H
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <complex>
#include <vector>

static std::vector<float> load_wav(const char *path, int *rate)
{
    std::vector<float> out;
    FILE *f = fopen(path, "rb");
    if(!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    fseek(f, 12, SEEK_SET);
    int bits = 16, ch = 1; long data_len = 0; *rate = 44100;
    for(;;)
    {
        unsigned char c[8];
        if(fread(c, 1, 8, f) != 8) break;
        const long sz = c[4] | (c[5]<<8) | (c[6]<<16) | ((long)c[7]<<24);
        if(!memcmp(c, "fmt ", 4))
        {
            unsigned char fmt[16]; if(fread(fmt, 1, 16, f) != 16) break;
            ch = fmt[2] | (fmt[3]<<8);
            *rate = fmt[4] | (fmt[5]<<8) | (fmt[6]<<16) | (fmt[7]<<24);
            bits = fmt[14] | (fmt[15]<<8);
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
        unsigned char b[8] = {0};
        if(fread(b, 1, (size_t)(bytes * ch), f) != (size_t)(bytes * ch)) break;
        if(bits == 24) { int v = b[0] | (b[1]<<8) | (b[2]<<16); if(v & 0x800000) v -= 0x1000000;
                         out.push_back((float)v / 8388608.0f); }
        else if(bits == 16) out.push_back((float)(int16_t)(b[0] | (b[1]<<8)) / 32768.0f);
        else out.push_back(0.0f);
    }
    fclose(f);
    return out;
}

/* trim leading silence: first sample over 10% of peak */
static void trim_onset(std::vector<float> &x)
{
    float pk = 0; for(float s : x) if(fabsf(s) > pk) pk = fabsf(s);
    size_t on = 0; while(on < x.size() && fabsf(x[on]) < pk * 0.10f) ++on;
    x.erase(x.begin(), x.begin() + (long)on);
}

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
static const double kBandLo = 120.0;   /* 1/3-octave from 120 Hz to ~20 kHz */
#define NENV 60                         /* 10 ms frames, up to 0.6 s... */
static const double kEnvFrame = 0.010;  /* ...or the reference length if shorter */

struct Profile { double band[NBANDS]; double crest[NBANDS]; double env[NENV]; int env_n; };

static void profile(const std::vector<float> &x, int rate, Profile &p, size_t env_len)
{
    const size_t N = 8192;
    double acc[NBANDS] = {0}, cacc[NBANDS] = {0};
    int windows = 0;
    /* short hits (a 130 ms kick) need at least one window: zero-pad */
    std::vector<float> xx = x;
    if(xx.size() < N + 1) xx.resize(N + 1, 0.0f);
    for(size_t off = 0; off + N <= xx.size(); off += N/2)
    {
        std::vector<std::complex<double> > buf(N);
        for(size_t i = 0; i < N; ++i)
            buf[i] = xx[off+i] * 0.5 * (1 - cos(2*M_PI*(double)i/(double)(N-1)));
        fft(buf);
        for(int b = 0; b < NBANDS; ++b)
        {
            const double lo = kBandLo * pow(2.0, b/3.0), hi = kBandLo * pow(2.0, (b+1)/3.0);
            const size_t i0 = (size_t)(lo * N / rate), i1 = std::min((size_t)(hi * N / rate), N/2);
            double e = 0; std::vector<double> m;
            for(size_t i = i0; i < i1; ++i) { const double a = std::abs(buf[i]); e += a*a; m.push_back(a); }
            acc[b] += e;
            if(m.size() >= 4)
            {
                std::vector<double> s = m; std::sort(s.begin(), s.end());
                cacc[b] += 20.0 * log10((s.back() + 1e-12) / (s[s.size()/2] + 1e-12));
            }
        }
        ++windows;
    }
    double tot = 0;
    for(int b = 0; b < NBANDS; ++b) { acc[b] /= (windows ? windows : 1); tot += acc[b]; }
    double top = -1e9;
    for(int b = 0; b < NBANDS; ++b)
    {
        p.band[b]  = 10.0 * log10(acc[b] / (tot > 0 ? tot : 1) + 1e-12);
        p.crest[b] = cacc[b] / (windows ? windows : 1);
        if(p.band[b] > top) top = p.band[b];
    }
    /* Band floor: 60 dB under the loudest band is the recording's analog
     * hiss (a 2.4 s tom take has a flat -60 dB floor across 20 of 28 bands)
     * or the synth's digital zero. Neither says anything about the voice, and
     * without this clamp the tom fitter chased noise floor with the tune pot. */
    for(int b = 0; b < NBANDS; ++b) if(p.band[b] < top - 60.0) p.band[b] = top - 60.0;
    /* envelope in absolute time over the reference's length */
    const size_t hop = (size_t)(kEnvFrame * rate);
    p.env_n = (int)std::min((size_t)NENV, env_len / hop);
    if(p.env_n < 2) p.env_n = 2;
    double pk = 1e-12;
    for(int i = 0; i < p.env_n; ++i)
    {
        double e = 0; size_t n = 0;
        for(size_t j = (size_t)i*hop; j < (size_t)(i+1)*hop; ++j, ++n) e += j < x.size() ? x[j]*x[j] : 0.0;
        p.env[i] = n ? sqrt(e/n) : 0.0;
        if(p.env[i] > pk) pk = p.env[i];
    }
    for(int i = 0; i < p.env_n; ++i) p.env[i] = std::max(-50.0, 20.0 * log10(p.env[i]/pk + 1e-6));
}

struct Score { double spectral, envelope, crest; double total() const { return spectral + 0.5*envelope + crest; } };

static Score score(const Profile &ref, const Profile &got)
{
    Score s = {0,0,0};
    for(int b = 0; b < NBANDS; ++b) { s.spectral += fabs(got.band[b] - ref.band[b]);
                                      s.crest    += fabs(got.crest[b] - ref.crest[b]); }
    s.spectral /= NBANDS; s.crest /= NBANDS;
    const int n = std::min(ref.env_n, got.env_n);
    for(int i = 0; i < n; ++i) s.envelope += fabs(got.env[i] - ref.env[i]);
    s.envelope /= (n ? n : 1);
    return s;
}
#endif
