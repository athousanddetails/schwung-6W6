#!/usr/bin/env bash
# A/B null test: build the engine from a git ref AND from the working tree with
# IDENTICAL flags in the same shell, render the same material from both, and
# report the residual.
#
# The doc's warning, learned in 9W9: a stale scratch copy or a differing -D
# turns into a phantom "difference" you then chase for an hour. Hence
# git archive (never a hand-copied tree) and one flag string used twice.
#
#   ./tools/ab_null.sh [ref]        default: HEAD
set -euo pipefail
REF="${1:-HEAD}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

FLAGS="-std=c++14 -O2"
INC="-Isrc -Isrc/dsp -Isrc/host -Isrc/vendor/606"

mkdir -p "$WORK/old"
git -C "$ROOT" archive "$REF" | tar -x -C "$WORK/old"

# the probe renders a fixed pattern to raw floats; it lives in the WORKING tree
# and is compiled against each engine in turn
cp "$ROOT/tools/ab_render.cpp" "$WORK/old/tools/" 2>/dev/null || true

build () {  # build <srcdir> <out>
    ( cd "$1" && clang++ $FLAGS $INC "$ROOT/tools/ab_render.cpp" \
        src/dsp/sd606_engine.cpp -o "$2" )
}
build "$WORK/old"  "$WORK/old.bin"
build "$ROOT"      "$WORK/new.bin"

"$WORK/old.bin" "$WORK/old.f32"
"$WORK/new.bin" "$WORK/new.f32"

python3 - "$WORK/old.f32" "$WORK/new.f32" "$REF" <<'PY'
import struct, sys, math
a=open(sys.argv[1],'rb').read(); b=open(sys.argv[2],'rb').read()
n=min(len(a),len(b))//4
A=struct.unpack("<%df"%n, a[:n*4]); B=struct.unpack("<%df"%n, b[:n*4])
same=sum(1 for x,y in zip(A,B) if x==y)
def rms(v): return math.sqrt(sum(x*x for x in v)/len(v)) if v else 0.0
d=[x-y for x,y in zip(A,B)]
ra, rd = rms(A), rms(d)
pa = max(abs(x) for x in A); pb = max(abs(x) for x in B)
print(f"  ref {sys.argv[3]} vs working tree, {n} samples")
print(f"  bit-identical samples : {same}/{n} ({100.0*same/n:.4f}%)")
if rd == 0.0:
    print("  residual              : EXACTLY ZERO — bit-identical output")
else:
    print(f"  residual (RMS)        : {20*math.log10(rd/ra):.1f} dB below the signal")
    print(f"  peak                  : {pa:.6f} -> {pb:.6f}  ({20*math.log10(pb/pa):+.3f} dB)")
    print(f"  rms                   : {ra:.6f} -> {rms(B):.6f}  ({20*math.log10(rms(B)/ra):+.3f} dB)")
PY
