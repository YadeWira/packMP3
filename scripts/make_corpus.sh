#!/bin/bash
# Build a fixed, reproducible MP3 corpus for packMP3 benchmarking.
# Mixes real-world samples with lame-generated variants covering the main
# MP3 axes (CBR/VBR, mono/stereo/joint, MPEG-1/2). Idempotent.
set -u
export LC_ALL=C

OUT="${1:-/home/forum/git/packMP3/corpus}"
REAL=/mnt/OSR_D3/fileFormatSamples/fileFormatSamples/audio/mp3
mkdir -p "$OUT"

command -v lame   >/dev/null || { echo "need lame";   exit 1; }
command -v ffmpeg >/dev/null || { echo "need ffmpeg"; exit 1; }

# --- real-world samples (MPEG-1 Layer III) ---
for f in example.mp3:real_128_stereo mp3-192.mp3:real_192_jstereo test2.mp3:real_224_32khz; do
  src="$REAL/${f%%:*}"; dst="$OUT/${f##*:}.mp3"
  [ -f "$src" ] && cp -f "$src" "$dst"
done

# --- generate variants from a real decoded source (25s) ---
SRC="$OUT/.src.wav"
ffmpeg -y -i "$REAL/mp3-192.mp3" -t 25 "$SRC" >/dev/null 2>&1 || { echo "ffmpeg decode failed"; exit 1; }

gen(){ lame --quiet "$@" "$SRC" "$OUT/$LBL.mp3"; }
LBL=gen_cbr128_stereo   gen -b 128
LBL=gen_cbr320_stereo   gen -b 320
LBL=gen_vbr_v0          gen -V 0
LBL=gen_vbr_v2          gen -V 2
LBL=gen_cbr128_mono     gen -m m -b 128
LBL=gen_cbr160_jstereo  gen -m j -b 160
LBL=gen_mpeg2_22k_64    gen --resample 22.05 -b 64
LBL=gen_mpeg2_16k_48    gen --resample 16 -b 48
rm -f "$SRC"

echo "Corpus in $OUT:"
for f in "$OUT"/*.mp3; do
  printf "  %-26s %8d bytes\n" "$(basename "$f")" "$(stat -c%s "$f")"
done
