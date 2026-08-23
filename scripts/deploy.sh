#!/usr/bin/env bash
#
# Install the 6W6 module on the Move SAFELY.
#
# Critical: never scp directly over a live dsp.so. The shim dlopen()s it into
# MoveOriginal, so overwriting the file mutates the mmap'd code pages of a
# running process — which segfaults the whole firmware. Upload to a temp name,
# then mv: rename(2) is atomic and leaves the old inode intact for the running
# process. New code is picked up when the slot next loads the module.
#
#   ./scripts/deploy.sh [host]      (default: move.local)
set -euo pipefail

HOST="${1:-move.local}"
SRC="$(cd "$(dirname "$0")/.." && pwd)"
DEST="/data/UserData/schwung/modules/sound_generators/6w6"
BUILD="$SRC/build/dsp.so"

[ -f "$BUILD" ] || { echo "no build/dsp.so — run ./scripts/build.sh sd606 first" >&2; exit 1; }

echo "==> $HOST:$DEST"
ssh "$HOST" "mkdir -p $DEST"

scp -q "$BUILD" "$HOST:$DEST/dsp.so.new"
scp -q "$SRC/src/module.json" "$HOST:$DEST/module.json.new"
for f in ui_chain.js web_ui.html help.json movy_config.json; do
    [ -f "$SRC/src/$f" ] && scp -q "$SRC/src/$f" "$HOST:$DEST/$f.new"
done

# Atomic swap. Do NOT replace this with a direct scp.
ssh "$HOST" "cd $DEST && for f in *.new; do mv -f \"\$f\" \"\${f%.new}\"; done && chmod 755 dsp.so && ls -l"

# The chain patch: gives the slot its capture rule (step buttons -> the
# built-in sequencer) so the editor works the moment 6W6 is picked.
if [ -f "$SRC/src/patches/6W6.json" ]; then
    scp -q "$SRC/src/patches/6W6.json" "$HOST:/data/UserData/schwung/patches/6W6.json.new"
    ssh "$HOST" "cd /data/UserData/schwung/patches && mv -f 6W6.json.new 6W6.json"
fi

# Prove what landed rather than assuming it did.
LOCAL_MD5=$(md5 -q "$BUILD" 2>/dev/null || md5sum "$BUILD" | cut -d' ' -f1)
REMOTE_MD5=$(ssh "$HOST" "md5sum $DEST/dsp.so | cut -d' ' -f1")
[ "$LOCAL_MD5" = "$REMOTE_MD5" ] || { echo "FATAL: md5 mismatch — $LOCAL_MD5 != $REMOTE_MD5" >&2; exit 1; }
echo "==> md5 verified: $LOCAL_MD5"

# A NEW dsp.so ON DISK IS NOT THE RUNNING ONE. The chain host dlopen()s the
# plugin into the shim; the atomic mv above swaps the directory entry while the
# running process keeps the old inode mapped, and `kill shadow_ui` does not
# help (different process). An on-device loadtest dlopens the file itself, so
# it passes against code nobody is hearing. Force the slot to reload.
if [ "${RELOAD:-1}" = "1" ]; then
    python3 "$SRC/scripts/reload_slot.py" "$HOST" "${SLOT:-0}" 6w6 \
        || echo "    (could not reload automatically - re-pick 6W6 in the slot)"
fi

echo "==> done."
