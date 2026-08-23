#!/bin/bash
# packMP3 corruption suite.
#
# Feeds damaged archives to the decompressor and checks that damage never
# turns into a crash, a hang, or -- as far as the format allows -- a silently
# wrong file.
#
# WHY THIS EXISTS, AND WHY IT DECLARES FIVE REGIMES INSTEAD OF ONE.
#
# Every corruption sweep run against packMP3 up to 2026-08-22 truncated by
# PERCENTAGE: 2%, 4%, ... 98%. Not one cut a handful of bytes off the end.
# That regime turned out to be the dangerous one and it was outside the whole
# test design, so no amount of care within the design could have found it:
#
#   truncated 1-60 bytes    v3.0e: 99 of 108 archives decoded to a WRONG file
#                                  with exit 0, no crash, no warning
#   truncated 1-60 percent  v3.0e: 45 crashes, 0 wrong files
#
# Small damage is the damage that gets through. Large damage breaks something
# the decoder notices and gets rejected; small damage stays consistent enough
# to decode into a plausible, wrong result. The same split showed up on the
# other axis -- single-bit flips produce 4x more silently-wrong output than
# 2-4 bit flips (76 vs 19 of 400, z=6.2) even though memory errors are flat
# across both (4 vs 5).
#
# So the regimes are declared here, in the harness, rather than chosen per
# run. A regime you have to remember to test is a regime you will eventually
# stop testing; the whole point is that the next person does not need to know
# any of the above. Credit to @LPJPG of the packJPG family, whose pre-declared
# pair of truncation formulations covered a regime they had not predicted
# either -- the declaration was there to stop them picking the flattering
# formulation after seeing results, and it paid off for an unrelated reason.
#
# WHAT COUNTS AS FAILURE. Crashes, hangs, and any REGRESSION in the count of
# silently-wrong outputs. Not the wrong outputs themselves: a few of them are
# irreducible. A .pm3 truncated by one byte can be a valid encoding of a
# DIFFERENT mp3 -- there is nothing inconsistent for any internal check to
# find, and the residue measured here is entirely of that kind (output of the
# exact original size, different bytes). Closing it needs a declared payload
# length or a checksum in the container, which is a format change. Until then
# the suite pins the number so it cannot quietly grow.
#
#   ./tests/corruption.sh                  normal run
#   PMP3_CORRUPT_QUICK=1 ...               fewer cases, for a fast check
#   PMP3_CORRUPT_BASELINE=<n> ...          expected silent-wrong count. The
#                                          default, 14, is the full run; a
#                                          --quick run scores lower and will
#                                          print a "fell to N" note, which is
#                                          the smaller grid and not progress.
#   PMP3_CORRUPT_CONTROL=<binary> ...      control: run an old build too, and
#                                          fail if it does NOT look worse
#
# Exit 0 only if everything passed.
set -u
export LC_ALL=C

HERE=$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )
ROOT=$( dirname "$HERE" )
BIN=${PMP3_BIN:-$ROOT/source/packMP3}
DATA=$HERE/data
WORK=$( mktemp -d )
trap 'rm -rf "$WORK"' EXIT

QUICK=${PMP3_CORRUPT_QUICK:-0}
BASELINE=${PMP3_CORRUPT_BASELINE:-14}
CONTROL=${PMP3_CORRUPT_CONTROL:-}

[ -x "$BIN" ] || { echo "no binary at $BIN -- run make first" >&2; exit 1; }
[ -d "$DATA" ] || bash "$HERE/make_testdata.sh" >/dev/null

if [ "$QUICK" = "1" ]; then
	BYTE_CUTS="1 2 5 13"; PCT_CUTS="5 25 60"; FLIP_SEEDS=40
else
	BYTE_CUTS="1 2 3 5 8 13 21 34 55"; PCT_CUTS="2 5 10 25 40 60 80"; FLIP_SEEDS=200
fi

# Sources: the generated corpus, minus anything that does not compress to a
# usable archive. Kept small on purpose -- this runs on every release.
SOURCES=$( ls "$DATA"/*.mp3 "$DATA"/*.mp2 2>/dev/null | head -6 )
[ -n "$SOURCES" ] || { echo "no test data" >&2; exit 1; }

# Written to a file rather than fed through a heredoc. The first version of
# this used <<-'PY', whose tab-stripping mangled the Python indentation; the
# generator then failed on every call, produced no damaged archives, and the
# suite reported a clean pass over two regimes that never ran. A harness that
# reports OK when a whole regime silently produced nothing is worse than no
# harness, so the emptiness check below exists as well.
cat > "$WORK/flip.py" <<'PY'
import sys, random
src, dst, seed, k = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
d = bytearray(open(src, 'rb').read())
r = random.Random(seed)
for _ in range(k):
    d[r.randrange(len(d))] ^= 1 << r.randrange(8)
open(dst, 'wb').write(d)
PY

flip_file() { # $1 archive $2 out $3 seed $4 nflips
	python3 "$WORK/flip.py" "$1" "$2" "$3" "$4" || return 1
	[ -s "$2" ] || return 1
}

# Classify one damaged archive. Echoes: crash | hang | reject | wrong | ident
classify() { # $1 binary $2 damaged archive $3 sha of the original mp3
	local bin=$1 arch=$2 ref=$3 out rc
	rm -rf "$WORK/out"; mkdir -p "$WORK/out"
	# BOTH streams captured: msgout is stdout by default, and a harness that
	# greps only stderr sees every rejection as silence.
	timeout 60 "$bin" x -o -np -od"$WORK/out" "$arch" >/dev/null 2>&1
	rc=$?
	[ $rc -ge 128 ] && { echo crash; return; }
	[ $rc -eq 124 ] && { echo hang; return; }
	out=$( ls "$WORK/out" 2>/dev/null | head -1 )
	[ -n "$out" ] && [ -s "$WORK/out/$out" ] || { echo reject; return; }
	if [ "$( sha256sum "$WORK/out/$out" | cut -d' ' -f1 )" = "$ref" ]; then
		echo ident
	else
		echo wrong
	fi
}

# Run every regime against one binary. Sets the g_* counters.
sweep() { # $1 binary $2 label
	local bin=$1 label=$2 f b arch ref full k pct seed verdict
	g_crash=0; g_hang=0; g_wrong=0; g_cells=0
	g_r1=0; g_r2=0; g_r3=0; g_r4=0
	for f in $SOURCES; do
		b=$( basename "$f" )
		rm -rf "$WORK/a"; mkdir -p "$WORK/a"
		"$bin" a -o -np -od"$WORK/a" "$f" >/dev/null 2>&1
		arch=$( ls "$WORK/a"/*.pm3 2>/dev/null | head -1 )
		[ -n "$arch" ] || continue
		ref=$( sha256sum "$f" | cut -d' ' -f1 )
		full=$( stat -c%s "$arch" )
		[ "$full" -gt 200 ] || continue

		# REGIME 1: bytes off the end. The one the old sweeps missed.
		for k in $BYTE_CUTS; do
			head -c $(( full - k )) "$arch" > "$WORK/d.pm3"
			verdict=$( classify "$bin" "$WORK/d.pm3" "$ref" ); g_cells=$(( g_cells + 1 )); g_r1=$(( g_r1 + 1 ))
			case $verdict in crash) g_crash=$(( g_crash + 1 ));; hang) g_hang=$(( g_hang + 1 ));;
				wrong) g_wrong=$(( g_wrong + 1 ));; esac
		done

		# REGIME 2: percentage of the file.
		for pct in $PCT_CUTS; do
			head -c $(( full * pct / 100 )) "$arch" > "$WORK/d.pm3"
			verdict=$( classify "$bin" "$WORK/d.pm3" "$ref" ); g_cells=$(( g_cells + 1 )); g_r2=$(( g_r2 + 1 ))
			case $verdict in crash) g_crash=$(( g_crash + 1 ));; hang) g_hang=$(( g_hang + 1 ));;
				wrong) g_wrong=$(( g_wrong + 1 ));; esac
		done
	done

	# REGIMES 3 and 4: bit flips on a full-length archive, one and several.
	# Both, because they do not fail the same way: one flip produces far more
	# silently-wrong output, several produce more crashes.
	rm -rf "$WORK/a"; mkdir -p "$WORK/a"
	set -- $SOURCES
	"$bin" a -o -np -od"$WORK/a" "$1" >/dev/null 2>&1
	arch=$( ls "$WORK/a"/*.pm3 2>/dev/null | head -1 )
	ref=$( sha256sum "$1" | cut -d' ' -f1 )
	if [ -n "$arch" ]; then
		for seed in $( seq 1 $FLIP_SEEDS ); do
			flip_file "$arch" "$WORK/d.pm3" "$seed" 1 || continue
			verdict=$( classify "$bin" "$WORK/d.pm3" "$ref" ); g_cells=$(( g_cells + 1 )); g_r3=$(( g_r3 + 1 ))
			case $verdict in crash) g_crash=$(( g_crash + 1 ));; hang) g_hang=$(( g_hang + 1 ));;
				wrong) g_wrong=$(( g_wrong + 1 ));; esac
			flip_file "$arch" "$WORK/d.pm3" $(( seed + 100000 )) 3 || continue
			verdict=$( classify "$bin" "$WORK/d.pm3" "$ref" ); g_cells=$(( g_cells + 1 )); g_r4=$(( g_r4 + 1 ))
			case $verdict in crash) g_crash=$(( g_crash + 1 ));; hang) g_hang=$(( g_hang + 1 ));;
				wrong) g_wrong=$(( g_wrong + 1 ));; esac
		done
	fi
	printf "  %-22s cells=%-5d crash=%-4d hang=%-4d silently-wrong=%d\n" \
		"$label" "$g_cells" "$g_crash" "$g_hang" "$g_wrong"
	printf "  %-22s per regime: bytes=%d percent=%d 1-flip=%d 3-flip=%d\n" \
		"" "$g_r1" "$g_r2" "$g_r3" "$g_r4"
	# An empty regime is a broken harness reporting a clean pass. This is not
	# hypothetical: the first version of this script mangled the flip
	# generator and sailed through both flip regimes without running a case.
	g_empty=""
	[ "$g_r1" -eq 0 ] && g_empty="$g_empty byte-cut"
	[ "$g_r2" -eq 0 ] && g_empty="$g_empty percent-cut"
	[ "$g_r3" -eq 0 ] && g_empty="$g_empty single-flip"
	[ "$g_r4" -eq 0 ] && g_empty="$g_empty multi-flip"
}

# REGIME 5: every guard by name.
#
# The four sweeps above measure the POPULATION. They do not pin any individual
# check, and a rare one can vanish without moving the totals: instrumenting
# sv_bound over the whole grid shows it never once leaves the valid range, and
# over 800 flipped archives it fires exactly twice, 0.25%. Delete that guard
# and the sweeps stay green -- while the archive that made it necessary writes
# 1102 bytes 524 bytes before a 580-byte buffer.
#
# So each guard gets a deterministic case that must produce ITS message. The
# seeds were found by scanning, not chosen; the flip generator is the same one
# the sweeps use, so the archives regenerate byte-identically from the corpus.
#
#   seed:flips  guard
#   2:1         coefficient magnitude past the Huffman table maximum
#   10:1        ancillary size outside the LAME prediction buffer
#   25:1        arithmetic decoder exhausted while reading main data
#   114:3       sv_bound outside the granule  <- the rare one, 3 in 1000
#   139:1       region sizes indexing past bandwidth_bounds[23]
#   364:1       Huffman table index with no model behind it
guards() { # $1 binary -- sets g_missing
	local bin=$1 arch ref spec seed k want out
	g_missing=""
	# Pinned to one named file: a seed only reproduces against the archive it
	# was found on. The first version took whichever source sorted first,
	# which was a different file, and reported all five guards missing.
	rm -rf "$WORK/a"; mkdir -p "$WORK/a"
	[ -f "$DATA/plain.mp3" ] || { g_missing=" (tests/data/plain.mp3 absent)"; return; }
	"$bin" a -o -np -od"$WORK/a" "$DATA/plain.mp3" >/dev/null 2>&1
	arch="$WORK/a/plain.pm3"
	[ -s "$arch" ] || { g_missing=" (could not build plain.pm3)"; return; }
	printf '%s\n' "2:1:coefficients" "10:1:ancillary size" "25:1:main data" \
		"114:3:sv_bound" "139:1:region sizes" "364:1:bad huffman table" | while IFS= read -r spec; do
		seed=${spec%%:*}; spec=${spec#*:}; k=${spec%%:*}; want=${spec#*:}
		flip_file "$arch" "$WORK/g.pm3" "$seed" "$k" || { echo "$want"; continue; }
		rm -rf "$WORK/out"; mkdir -p "$WORK/out"
		# stdout AND stderr: msgout is stdout by default.
		out=$( timeout 60 "$bin" x -o -np -od"$WORK/out" "$WORK/g.pm3" 2>&1 )
		case $out in *"$want"*) ;; *) echo "$want";; esac
	done > "$WORK/missing.txt"
	g_missing=$( tr '\n' ',' < "$WORK/missing.txt" | sed 's/,$//; s/,/, /g' )
}

echo "packMP3 corruption suite -- five regimes, declared here rather than chosen per run"
echo
sweep "$BIN" "$( basename "$BIN" )"
new_crash=$g_crash; new_hang=$g_hang; new_wrong=$g_wrong; new_empty=$g_empty
guards "$BIN"
if [ -z "$g_missing" ]; then
	echo "                         every guard fired on its own case (6/6)"
else
	echo "                         GUARDS NOT REACHED: $g_missing"
fi

ctl_ok=1
if [ -n "$CONTROL" ] && [ -x "$CONTROL" ]; then
	sweep "$CONTROL" "control: $( basename "$CONTROL" )"
	# A suite that cannot tell a fixed build from a broken one is not
	# measuring anything. If an older build does not look worse, the
	# harness is the thing that is wrong.
	if [ "$g_crash" -le "$new_crash" ] && [ "$g_wrong" -le "$new_wrong" ]; then
		echo
		echo "  CONTROL FAILED: the older build did not look worse than the current one."
		echo "  Either the control binary is not actually older, or this suite is not"
		echo "  measuring what it claims. Do not trust the run above."
		ctl_ok=0
	fi
fi

echo
fail=0
[ "$new_crash" -gt 0 ] && { echo "  FAIL: $new_crash crash(es)"; fail=1; }
[ "$new_hang"  -gt 0 ] && { echo "  FAIL: $new_hang hang(s)"; fail=1; }
if [ "$new_wrong" -gt "$BASELINE" ]; then
	echo "  FAIL: silently-wrong outputs rose to $new_wrong (baseline $BASELINE)"
	echo "        Raise PMP3_CORRUPT_BASELINE only with a measurement showing why."
	fail=1
elif [ "$new_wrong" -lt "$BASELINE" ]; then
	echo "  note: silently-wrong outputs fell to $new_wrong (baseline $BASELINE)."
	echo "        Lower PMP3_CORRUPT_BASELINE in this file to lock the improvement in."
fi
[ "$ctl_ok" = "0" ] && fail=1
if [ -n "$g_missing" ]; then
	echo "  FAIL: guard(s) did not fire on the case that requires them: $g_missing"
	echo "        A population sweep cannot see a rare guard disappear."
	fail=1
fi
if [ -n "$new_empty" ]; then
	echo "  FAIL: regime(s) produced no cases:$new_empty"
	echo "        A pass over a regime that never ran is not a pass."
	fail=1
fi

if [ "$fail" = "0" ]; then
	echo "  OK -- no crashes, no hangs, silently-wrong within baseline ($new_wrong <= $BASELINE)"
	echo
	echo "  The residue is expected and irreducible without a format change: a"
	echo "  truncated archive can be a valid encoding of a different mp3, and no"
	echo "  internal check can see that. It needs a declared length or a checksum."
fi
exit $fail
