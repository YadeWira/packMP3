#!/bin/bash
# packMP3 benchmark scale: ratio + encode ms + decode ms + lossless verify,
# per file and averaged. A/B spine for the 3-axis optimization work.
#
# Usage: bench_mp3.sh [corpus_dir] [out_label]
#   corpus_dir : dir of .mp3 files (default ./corpus)
#   out_label  : tag printed in the header (e.g. "baseline", "lto")
set -u
export LC_ALL=C

CORPUS="${1:-/home/forum/git/packMP3/corpus}"
LABEL="${2:-run}"
BIN=/home/forum/git/packMP3/source/packMP3
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
REP=3   # take best-of-N for timing stability

[ -x "$BIN" ] || { echo "build packMP3 first"; exit 1; }

# best-of-REP wall time in ms for a command
tms(){ local b=999999 s e t i; for i in $(seq $REP); do
  s=$(date +%s%N); eval "$1" >/dev/null 2>&1; e=$(date +%s%N)
  t=$(( (e-s)/1000000 )); [ $t -lt $b ] && b=$t; done; echo $b; }

printf "== packMP3 bench [%s] ==\n" "$LABEL"
printf "%-26s %9s | %6s | %5s | %5s | %s\n" "file" "bytes" "ratio" "enc" "dec" "lossless"
printf '%.0s-' {1..72}; echo

sr=0; se=0; sd=0; n=0; allok=1
for src in "$CORPUS"/*.mp3; do
  [ -f "$src" ] || continue
  bn=$(basename "$src" .mp3); o=$(stat -c%s "$src")
  cp -f "$src" "$W/in.mp3"; rm -f "$W/in.pm3" "$W/d/in.mp3"; mkdir -p "$W/d"
  et=$(tms "$BIN a -np -o -od$W $W/in.mp3")
  c=0; [ -f "$W/in.pm3" ] && c=$(stat -c%s "$W/in.pm3")
  dt=$(tms "$BIN x -np -o -od$W/d $W/in.pm3")
  # lossless: -ver compresses+decodes+compares; ok => errorlevel 0
  if $BIN a -np -o -ver -od$W/v "$W/in.mp3" >/dev/null 2>&1; then lossok="OK"; else lossok="FAIL"; allok=0; fi
  if [ "$c" -gt 0 ]; then pct=$(awk "BEGIN{printf \"%.1f\",100*$c/$o}"); else pct="FAIL"; allok=0; fi
  printf "%-26s %9d | %5s%% | %4sms | %4sms | %s\n" "$bn" "$o" "$pct" "$et" "$dt" "$lossok"
  sr=$(awk "BEGIN{print $sr+$pct}"); se=$((se+et)); sd=$((sd+dt)); n=$((n+1))
done
printf '%.0s-' {1..72}; echo
[ $n -gt 0 ] && printf "%-26s %9s | %5s%% | %4sms | %4sms | %s\n" "PROMEDIO ($n)" "" \
  "$(awk "BEGIN{printf \"%.1f\",$sr/$n}")" "$(awk "BEGIN{printf \"%.0f\",$se/$n}")" \
  "$(awk "BEGIN{printf \"%.0f\",$sd/$n}")" "$([ $allok -eq 1 ] && echo ALL-OK || echo HAS-FAIL)"
