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

# Prove what landed rather than assuming it did.
LOCAL_MD5=$(md5 -q "$BUILD" 2>/dev/null || md5sum "$BUILD" | cut -d' ' -f1)
REMOTE_MD5=$(ssh "$HOST" "md5sum $DEST/dsp.so | cut -d' ' -f1")
[ "$LOCAL_MD5" = "$REMOTE_MD5" ] || { echo "FATAL: md5 mismatch — $LOCAL_MD5 != $REMOTE_MD5" >&2; exit 1; }
echo "==> md5 verified: $LOCAL_MD5"
echo "==> done. Reload the slot (or kill shadow_ui) to pick up new code."
