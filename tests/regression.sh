#!/bin/bash
# packMP3 regression suite.
#
# Checks two independent things, and keeps them independent on purpose:
#
#   1. REVERSIBILITY -- compress, decompress, compare byte for byte.
#   2. THE CODECS ACTUALLY RAN -- for each file, that every codec path it is
#      supposed to exercise left its own trace in the output.
#
# The second exists because the first cannot see a dead codec. packMP3 falls
# back to storing data verbatim when a codec does not help, and cover-art
# recompression bails out silently on anything it does not like. Storing
# verbatim is perfectly reversible, so a build where every codec had stopped
# working would still round-trip every file byte-exact and pass a suite that
# only checked reversibility. The failure would be invisible: no error, no
# crash, only a worse ratio nobody was watching.
#
# Idea and structure adapted from ytool's regression.sh, which hit the same
# problem from the other side (a precomp plugin that silently did nothing
# while the suite stayed green).
#
#   ./tests/regression.sh                       normal run
#   PMP3_ALLOW_MISSING_CODECS=1 ...             skip check 2 (prints a warning)
#   PMP3_SIMULATE_DEAD=cover ...                control: prove check 2 fires
#   PMP3_SIMULATE_DEAD=chunks ...               control: same, other codec
#
# Exit 0 only if everything passed.
set -u
export LC_ALL=C

HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="${PMP3_BIN:-$HERE/../source/packMP3}"
DATA="${PMP3_DATA:-$HERE/data}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

ALLOW_MISSING="${PMP3_ALLOW_MISSING_CODECS:-0}"
SIMULATE_DEAD="${PMP3_SIMULATE_DEAD:-}"

[ -x "$BIN" ] || { echo "no binary at $BIN (build it first, or set PMP3_BIN)" >&2; exit 2; }
[ -d "$DATA" ] || { echo "no corpus at $DATA -- run tests/make_testdata.sh" >&2; exit 2; }

# file | codec paths it must exercise | extra flags
#
# The signal is per CODEC PATH, not per file, and the three paths do not
# signal the same way -- that is the whole difficulty. See check_signal().
CASES="
plain.mp3        | audio          |
mpeg2_vbr.mp3    | audio          |
layer2.mp2       | audio          |
layer2_junk.mp2  | audio          |
cover_jpg.mp3    | audio,cover    |
cover_png.mp3    | audio,cover    |
plain.mp3        | audio,chunks   | -k4
"

# A codec path has "signalled" if it left its own trace. Three different
# mechanisms, established by diffing a healthy run against a run with that
# path disabled -- not by guessing which line looked relevant:
#
#   audio  -- ABSENCE of the no-gain note. packMP3 only says "no size gain"
#             when the result is not smaller, so its presence means the coder
#             produced nothing useful.
#   cover  -- PRESENCE of the cover-art line. A cover that is not recompressed
#             produces no line at all, so absence is the failure.
#   chunks -- PRESENCE of a chunk count, and it is NOT in the compress output:
#             -k1 and -k4 print identical lines apart from ratio and timing.
#             It has to be read back from the archive with `list`, which is
#             better than a message anyway: it comes from the container, and
#             breaking it would break decoding rather than pass silently.
check_signal() {
    local path="$1" out="$2" archive="$3"
    case "$path" in
        audio)  grep -q 'no size gain' <<<"$out" && return 1; return 0 ;;
        cover)  grep -q 'cover art (' <<<"$out" && return 0; return 1 ;;
        chunks)
            local n
            n=$("$BIN" list -np "$archive" 2>/dev/null \
                | sed -n 's/.*chunks *: *\([0-9]\+\).*/\1/p' | head -1)
            [ -n "$n" ] && [ "$n" -ge 2 ] && return 0
            return 1 ;;
        *) echo "unknown codec path '$path'" >&2; exit 2 ;;
    esac
}

pass=0; fail=0; dead=0
printf '%-18s %-8s %-14s %s\n' FILE FLAGS RESULT DETAIL
printf '%s\n' '--------------------------------------------------------------'

while IFS='|' read -r file paths flags; do
    file="$(tr -d ' ' <<<"${file:-}")"
    [ -z "$file" ] && continue
    paths="$(tr -d ' ' <<<"$paths")"
    flags="$(tr -d ' ' <<<"$flags")"

    src="$DATA/$file"
    [ -f "$src" ] || { printf '%-18s %-8s %-14s %s\n' "$file" "$flags" '*** MISSING ***' "$src"; fail=$((fail+1)); continue; }

    # The control. Disabling a codec with its own documented flag is a real
    # simulation of that codec dying: the rest of the run is untouched, so a
    # failure can only come from the missing signal.
    extra=""
    case "$SIMULATE_DEAD" in
        cover)  extra="-nc" ;;                          # skip cover recompression
        chunks) [ "$flags" = "-k4" ] && flags="-k1" ;;  # one chunk = no chunk table
        "")     ;;
        *) echo "PMP3_SIMULATE_DEAD must be cover or chunks" >&2; exit 2 ;;
    esac

    d="$WORK/$RANDOM$RANDOM"; mkdir -p "$d/in" "$d/out"
    cp "$src" "$d/in/" || exit 2
    base="${file%.*}"

    # shellcheck disable=SC2086
    out=$("$BIN" a -o -np $flags $extra -od"$d" "$d/in/$file" 2>&1)
    archive="$d/$base.pm3"
    if [ ! -s "$archive" ]; then
        printf '%-18s %-8s %-14s %s\n' "$file" "$flags" '*** FAIL ***' 'no archive produced'
        fail=$((fail+1)); continue
    fi

    "$BIN" x -o -np -od"$d/out" "$archive" >/dev/null 2>&1
    back=$(find "$d/out" -type f | head -1)
    if [ -z "$back" ] || ! cmp -s "$src" "$back"; then
        printf '%-18s %-8s %-14s %s\n' "$file" "$flags" '*** FAIL ***' 'not byte-exact'
        fail=$((fail+1)); continue
    fi

    if [ "$ALLOW_MISSING" = "1" ]; then
        printf '%-18s %-8s %-14s %s\n' "$file" "$flags" 'OK' 'reversible (codecs unchecked)'
        pass=$((pass+1)); continue
    fi

    missing=""
    for p in ${paths//,/ }; do
        check_signal "$p" "$out" "$archive" || missing="$missing $p"
    done

    if [ -n "$missing" ]; then
        # Deliberately NOT counted as FAIL. "Reversible but did nothing" and
        # "not reversible" send you to different places in the code.
        printf '%-18s %-8s %-14s %s\n' "$file" "$flags" '*** CODEC DEAD ***' "no signal from:$missing"
        dead=$((dead+1))
    else
        printf '%-18s %-8s %-14s %s\n' "$file" "$flags" 'OK' "$paths"
        pass=$((pass+1))
    fi
done <<<"$CASES"

echo
if [ "$ALLOW_MISSING" = "1" ]; then
    echo "WARNING: PMP3_ALLOW_MISSING_CODECS=1 -- this run did NOT verify that"
    echo "         any codec actually ran. Reversibility only."
fi
[ -n "$SIMULATE_DEAD" ] && echo "NOTE: PMP3_SIMULATE_DEAD=$SIMULATE_DEAD -- control run, failures are expected."
echo "$pass ok, $fail failed, $dead codec-dead"
[ "$fail" -eq 0 ] && [ "$dead" -eq 0 ] && exit 0
exit 1
