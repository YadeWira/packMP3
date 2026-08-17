#!/bin/bash
# Build the codec-coverage corpus for tests/regression.sh.
#
# Deliberately small and fully synthetic: every file is generated here, so the
# suite runs anywhere without depending on a sample collection that may not
# exist on the machine. This is NOT the benchmarking corpus -- that one lives
# in scripts/make_corpus.sh and optimises for ratio measurement. This one
# optimises for one thing: at least one file per codec path, so a dead codec
# has somewhere to show up.
#
# Idempotent. Regenerates only what is missing unless -f is given.
set -u
export LC_ALL=C

OUT="${1:-}"
[ -z "$OUT" ] && OUT="$(cd "$(dirname "$0")" && pwd)/data"
FORCE=0
[ "${2:-}" = "-f" ] && FORCE=1

for t in ffmpeg lame; do
    command -v "$t" >/dev/null || { echo "need $t" >&2; exit 1; }
done
mkdir -p "$OUT" || exit 1

have() { [ "$FORCE" = 0 ] && [ -s "$OUT/$1" ]; }

# Source audio. Noise plus a tone: noise keeps the coder from finding a
# degenerate best case, the tone keeps it from being pure entropy.
SRC="$OUT/.src.wav"
if ! have .src.wav; then
    ffmpeg -v error -y -f lavfi \
        -i "sine=frequency=440:duration=8,aeval='sin(3*PI*t)*random(0)*0.4|cos(2*PI*t)*random(1)*0.4'" \
        -ac 2 -ar 44100 "$SRC" || exit 1
fi

# --- Layer III, no tag: exercises packMP3's own coder and nothing else ---
have plain.mp3      || lame --quiet -b 192 "$SRC" "$OUT/plain.mp3"      || exit 1

# --- Layer III, low sample rate + VBR: MPEG-2 Layer III, the shape that
#     v3.0c fixed. Kept so a regression there fails loudly again. ---
have mpeg2_vbr.mp3  || lame --quiet -V9 --resample 22.05 "$SRC" "$OUT/mpeg2_vbr.mp3" || exit 1

# --- Layer II: routed to the packMP2 backend, a different codec entirely ---
have layer2.mp2     || ffmpeg -v error -y -i "$SRC" -c:a mp2 -b:a 128k "$OUT/layer2.mp2" || exit 1

# --- Cover art. Two files, because JPEG goes through packJPG and PNG through
#     packPNG: they are separate codecs and a dead one must not hide behind
#     the other. testsrc2 gives real image structure, not flat colour, so the
#     recompressors have something to do. ---
if ! have cover_jpg.mp3 || ! have cover_png.mp3; then
    ffmpeg -v error -y -f lavfi -i "testsrc2=size=640x480:duration=1:rate=1" \
        -frames:v 1 -q:v 3 "$OUT/.cov.jpg" || exit 1
    ffmpeg -v error -y -f lavfi -i "testsrc2=size=640x480:duration=1:rate=1" \
        -frames:v 1 "$OUT/.cov.png" || exit 1
    for k in jpg png; do
        ffmpeg -v error -y -i "$OUT/plain.mp3" -i "$OUT/.cov.$k" \
            -map 0:a -map 1:v -c copy -id3v2_version 3 \
            -metadata:s:v title="Album cover" -disposition:v attached_pic \
            "$OUT/cover_$k.mp3" || exit 1
    done
fi

# --- Layer II preceded by junk: the first frame lands past the detection
#     window, which is what v3.0c fixed. 20000 > the 8192 peek window and
#     < the 64 KB tolerance. ---
if ! have layer2_junk.mp2; then
    head -c 20000 /dev/zero > "$OUT/layer2_junk.mp2" 2>/dev/null
    cat "$OUT/layer2.mp2" >> "$OUT/layer2_junk.mp2"
fi

echo "corpus in $OUT:"
for f in "$OUT"/*.mp3 "$OUT"/*.mp2; do
    [ -e "$f" ] && printf "  %-18s %8s bytes\n" "$(basename "$f")" "$(stat -c%s "$f")"
done

# Known gap, stated rather than hidden: no Layer I (MP1) file. ffmpeg has no
# Layer I encoder and lame/twolame are Layer III/II only, so one cannot be
# synthesised here. The MP1 path is therefore NOT covered by this suite.
# Same for a Layer II file that legitimately does not shrink -- the one real
# example on hand is a third-party sample, and every synthetic MP2 tried
# compressed fine. That case exercises packMP2's never-expand fallback, so
# the "audio" signal for it is inverted and it needs its own table entry if a
# suitable file is ever added.
echo
echo "not covered: MP1 (no encoder available), Layer II verbatim-store fallback"
