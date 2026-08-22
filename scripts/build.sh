#!/usr/bin/env bash
# Build the 6W6 module for Ableton Move (aarch64), or natively with NATIVE=1.
#
# Runs on the Trash VPS, not the Mac (no toolchain here). Docker may not be in
# the login shell's groups there — DOCKER="sg docker -c" works around it.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="schwung-6w6-builder"
TARGET="${1:-all}"

if [ ! -f /.dockerenv ]; then
    echo "=== 6W6 build (Docker, ubuntu:22.04 / glibc 2.35) ==="
    docker image inspect "$IMAGE_NAME" >/dev/null 2>&1 || \
        docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$REPO_ROOT"
    docker run --rm -v "$REPO_ROOT:/build" -u "$(id -u):$(id -g)" -w /build \
        -e "NATIVE=${NATIVE:-0}" "$IMAGE_NAME" ./scripts/build.sh "$TARGET"
    exit 0
fi

cd "$REPO_ROOT"
python3 scripts/gen_params.py

if [ "${NATIVE:-0}" = "1" ]; then
    BUILD_DIR="build-native"
    echo "mode: NATIVE x86_64 (reference comparison only)"
    cmake -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release
else
    BUILD_DIR="build"
    cmake -B "$BUILD_DIR" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake -DCMAKE_BUILD_TYPE=Release
fi
cmake --build "$BUILD_DIR" --target "$TARGET" -j"$(nproc)"

# ---- glibc gate: anything above 2.35 will not load on the Move ----
if [ "${NATIVE:-0}" != "1" ] && [ -f "$BUILD_DIR/dsp.so" ]; then
    MAXV=$(aarch64-linux-gnu-objdump -T "$BUILD_DIR/dsp.so" \
           | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -1)
    echo "max glibc symbol: ${MAXV:-none}"
    case "${MAXV:-GLIBC_2.0}" in
        GLIBC_2.3[0-5]|GLIBC_2.[0-2]*|GLIBC_2.[0-9]|GLIBC_2.[12][0-9]|none) ;;
        *) echo "FATAL: $MAXV exceeds the Move's glibc 2.35" >&2; exit 1;;
    esac
fi

# ---- Package for the Module Store ----
if [ "${NATIVE:-0}" != "1" ] && [ -f "$BUILD_DIR/dsp.so" ]; then
    rm -rf dist/6w6
    mkdir -p dist/6w6
    cp "$BUILD_DIR/dsp.so"  dist/6w6/
    cp src/module.json      dist/6w6/
    cp src/ui_chain.js      dist/6w6/ 2>/dev/null || true
    cp src/web_ui.html      dist/6w6/ 2>/dev/null || true
    cp src/help.json        dist/6w6/ 2>/dev/null || true
    cp src/movy_config.json dist/6w6/ 2>/dev/null || true
    (cd dist && tar -czf 6w6-module.tar.gz 6w6/)
    echo "Tarball: dist/6w6-module.tar.gz"
fi

echo; echo "=== Artifacts ==="
find "$BUILD_DIR" -maxdepth 1 -type f \( -name "*.so" -o -name "sd606_*" \) \
     -exec sh -c 'printf "%s\n  " "$1"; file -b "$1"' _ {} \;
