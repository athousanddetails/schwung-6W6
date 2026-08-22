#!/usr/bin/env python3
"""Fit a metallic partial table from a hardware drum-machine recording.

This is the method the upstream repo used for the hi-hats, written down and
made repeatable: run FFTs at several points through the decay and keep the
peaks that STAY PUT as it dies. A resonant metal line persists; noise and
strike transients do not.

    ./fit_partials.py <wav> [--name kCymbalPartials] [--max 64]
    ./fit_partials.py <wav> --validate          # score against the known
                                                # open-hat table

Emits a C++ Partial table in the vendored HiHats.hpp format, plus the envelope
measurements a HiHatSpec needs. Pure standard library on purpose: no numpy on
the authoring machine and this must stay runnable years from now.
"""
import argparse, cmath, math, struct, sys, wave

# Fecher's measured open-hat table, for --validate.
KNOWN_OH = [3804,4270,4500,4632,4800,4900,5140,5400,5500,5610,5978,6340,6500,
            6600,6701,7106,7200,7339,7500,7686,7854,7938,8048,8399,8514,8601,
            8700,8876,9000,9100,9394,9500,9600,9756,9899,10143,10300,10800,
            11201,11571,11673,12318,12680,12841,13340,13663,16122]


def read_wav_mono(path):
    w = wave.open(path, 'rb')
    ch, width, rate, n = (w.getnchannels(), w.getsampwidth(),
                          w.getframerate(), w.getnframes())
    raw = w.readframes(n)
    w.close()
    out = []
    if width == 3:                                   # 24-bit little-endian
        for i in range(0, len(raw), 3 * ch):
            b = raw[i:i+3]
            v = b[0] | (b[1] << 8) | (b[2] << 16)
            if v & 0x800000: v -= 0x1000000
            out.append(v / 8388608.0)
    elif width == 2:
        for v in struct.unpack('<%dh' % (len(raw)//2), raw)[::ch]:
            out.append(v / 32768.0)
    elif width == 4:
        for v in struct.unpack('<%di' % (len(raw)//4), raw)[::ch]:
            out.append(v / 2147483648.0)
    else:
        sys.exit("unsupported sample width: %d bytes" % width)
    return out, rate


def fft(a):
    """Iterative radix-2 FFT. Input length must be a power of two."""
    n = len(a)
    if n & (n - 1): raise ValueError("length must be a power of two")
    j = 0
    a = list(a)
    for i in range(1, n):                            # bit reversal
        bit = n >> 1
        while j & bit:
            j ^= bit; bit >>= 1
        j |= bit
        if i < j: a[i], a[j] = a[j], a[i]
    length = 2
    while length <= n:
        ang = -2j * math.pi / length
        wl = cmath.exp(ang)
        for i in range(0, n, length):
            w = 1 + 0j
            half = length >> 1
            for k in range(half):
                u = a[i + k]; v = a[i + k + half] * w
                a[i + k] = u + v
                a[i + k + half] = u - v
                w *= wl
        length <<= 1
    return a


def spectrum(samples, start, size, rate):
    """Hann-windowed magnitude spectrum starting at `start`."""
    seg = samples[start:start + size]
    if len(seg) < size: seg = seg + [0.0] * (size - len(seg))
    win = [seg[i] * 0.5 * (1 - math.cos(2 * math.pi * i / (size - 1)))
           for i in range(size)]
    spec = fft(win)
    return [abs(spec[i]) for i in range(size // 2)]


def peaks(mag, rate, size, floor_db=-58.0, lo_hz=150.0, hi_hz=17000.0):
    """Local maxima above a floor, refined by parabolic interpolation."""
    top = max(mag) or 1.0
    floor = top * (10.0 ** (floor_db / 20.0))
    bin_hz = rate / size
    out = []
    for i in range(2, len(mag) - 2):
        f = i * bin_hz
        if f < lo_hz or f > hi_hz: continue
        m = mag[i]
        if m <= floor: continue
        if not (m > mag[i-1] and m >= mag[i+1]): continue
        a, b, c = mag[i-1], m, mag[i+1]
        denom = a - 2*b + c
        delta = 0.5 * (a - c) / denom if denom else 0.0
        out.append(((i + delta) * bin_hz, b / top))
    return out


def fit(path, max_partials=64, windows=6, size=16384, persist=0.5,
        tol_cents=45.0):
    samples, rate = read_wav_mono(path)
    peak_amp = max(abs(s) for s in samples) or 1.0
    onset = next((i for i, s in enumerate(samples)
                  if abs(s) > peak_amp * 0.10), 0)
    usable = len(samples) - onset - size
    if usable < 0:
        sys.exit("file too short for a %d-point window" % size)

    # Spread the analysis windows across the decay. The first sits just after
    # the strike; the last as deep into the tail as the file allows.
    offsets = [onset + int(usable * (k / max(1, windows - 1)))
               for k in range(windows)]

    seen = []          # [freq, [amps per window], hit count]
    for wi, off in enumerate(offsets):
        for f, a in peaks(spectrum(samples, off, size, rate), rate, size):
            for entry in seen:
                if abs(1200 * math.log2(f / entry[0])) < tol_cents:
                    entry[1].append(a); entry[2] += 1
                    entry[0] = (entry[0] * (entry[2] - 1) + f) / entry[2]
                    break
            else:
                seen.append([f, [a], 1])

    need = max(2, int(math.ceil(windows * persist)))
    kept = [e for e in seen if e[2] >= need]
    kept.sort(key=lambda e: -max(e[1]))
    kept = kept[:max_partials]
    kept.sort(key=lambda e: e[0])
    return kept, rate, samples, onset, peak_amp


def envelope(samples, onset, rate, peak_amp):
    """Decay measurements a HiHatSpec needs: -60 dB time and total duration."""
    def rms_at(t):
        i = onset + int(t * rate); n = int(0.010 * rate)
        seg = samples[i:i+n]
        if not seg: return 0.0
        return math.sqrt(sum(s*s for s in seg) / len(seg))
    a0 = rms_at(0.005) or 1e-9
    t60 = None
    t = 0.005
    while t < (len(samples) - onset) / rate - 0.011:
        if rms_at(t) < a0 * 0.001: t60 = t; break
        t += 0.005
    total = (len(samples) - onset) / rate
    return t60, total


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav")
    ap.add_argument("--name", default="kFittedPartials")
    ap.add_argument("--max", type=int, default=64)
    ap.add_argument("--windows", type=int, default=6)
    ap.add_argument("--bell", type=int, default=3,
                    help="how many of the loudest lines get the strike accent")
    ap.add_argument("--validate", action="store_true",
                    help="score the fit against Fecher's open-hat table")
    args = ap.parse_args()

    kept, rate, samples, onset, peak_amp = fit(
        args.wav, args.max, args.windows)
    t60, total = envelope(samples, onset, rate, peak_amp)

    print("// source: %s" % args.wav.split("/")[-1], file=sys.stderr)
    print("// %d partials kept, %.1f..%.1f Hz" %
          (len(kept), kept[0][0], kept[-1][0]), file=sys.stderr)
    print("// decay to -60 dB: %s, file length after onset: %.3f s" %
          ("%.3f s" % t60 if t60 else "beyond the file", total), file=sys.stderr)
    print("// referenceDurationSeconds = %.4f  (%d frames / %d)" %
          (total, int(total * rate), rate), file=sys.stderr)

    if args.validate:
        got = [e[0] for e in kept]
        matched, errs = 0, []
        for k in KNOWN_OH:
            best = min(got, key=lambda g: abs(g - k))
            cents = abs(1200 * math.log2(best / k))
            if cents < 60: matched += 1; errs.append(cents)
        print("\nVALIDATION against the measured open-hat table:", file=sys.stderr)
        print("  %d/%d known lines recovered within 60 cents" %
              (matched, len(KNOWN_OH)), file=sys.stderr)
        if errs:
            print("  median error %.1f cents, worst %.1f cents" %
                  (sorted(errs)[len(errs)//2], max(errs)), file=sys.stderr)
        return

    amps = [max(e[1]) for e in kept]
    top = max(amps) or 1.0
    bell_cut = sorted(amps, reverse=True)[:args.bell][-1] if kept else 1.0
    print("static constexpr Partial %s[] = {" % args.name)
    for e in kept:
        a = max(e[1]) / top
        bell = ", true" if max(e[1]) >= bell_cut else ""
        print("    {%8.1ff, %6.3ff%s }," % (e[0], a, bell))
    print("};")
    print("static constexpr int %sCount = %d;" % (args.name, len(kept)))


if __name__ == "__main__":
    main()
