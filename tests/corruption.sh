#!/bin/bash
# packMP3 corruption suite.
#
# Feeds damaged archives to the decompressor and checks that damage never
# turns into a crash, a hang, or -- as far as the format allows -- a silently
# wrong file.
#
# WHY THIS EXISTS, AND WHY IT DECLARES SIX REGIMES INSTEAD OF ONE.
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
#   PMP3_CORRUPT_TIMEOUT=0.01 ...          control: prove the hang classifier
#                                          fires (expect ~109 hangs and a FAIL)
#   PMP3_CORRUPT_SLOW_FACTOR=1 ...         control: prove the timing axis
#                                          discriminates (expect ~34 slow cells)
#
# Run against a sanitizer build (`make corrupt-asan`) and the guard regime also
# checks that each guard sits UPSTREAM of the access it protects -- rejecting
# after an out-of-range read looks identical in the verdict alone.
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
# Corrupt mp3s that compress and then cannot be decompressed. Pre-existing and
# not silent: both cases measured here CRASHED v3.0e and are clean rejections
# now. Pinned so the count cannot grow unnoticed.
UNREADABLE_BASELINE=${PMP3_CORRUPT_UNREADABLE:-2}
CONTROL=${PMP3_CORRUPT_CONTROL:-}
# Per-case timeout. Configurable so the hang classifier can be PROVEN to fire:
# PMP3_CORRUPT_TIMEOUT=0.01 makes every normal decode overrun and the suite
# fails with 109 hangs. The value matters -- the first control used 1 second
# against a decode that takes 95 ms, reported zero hangs, and looked exactly
# like a broken classifier. A control that cannot produce the condition it is
# testing for is not a control. A classifier nobody has seen fire is worth what any unproven check
# is worth -- and the hang class matters precisely because sanitizers are
# silent on it. @PJPG measured a single header byte that leaves ASAN and UBSan
# with nothing to say and takes a decode from 431 ms to over 25 seconds; only a
# timeout sees that.
TIMEOUT=${PMP3_CORRUPT_TIMEOUT:-60}
# A cell is "slow" at this multiple of the median healthy decode. Settable so
# the axis can be PROVEN to discriminate: at 1 roughly every cell qualifies,
# which is what a working measurement must show.
SLOW_FACTOR=${PMP3_CORRUPT_SLOW_FACTOR:-10}
# Below this fraction of the baseline, a cell that PRODUCED OUTPUT is
# implausibly fast -- it cannot have decoded anything. Settable so this end
# can be proven to discriminate too: at 100 every cell qualifies.
FAST_DIV=${PMP3_CORRUPT_FAST_DIV:-8}

[ -x "$BIN" ] || { echo "no binary at $BIN -- run make first" >&2; exit 1; }
[ -d "$DATA" ] || bash "$HERE/make_testdata.sh" >/dev/null

if [ "$QUICK" = "1" ]; then
	BYTE_CUTS="1 2 5 13"; PCT_CUTS="5 25 60"; FLIP_SEEDS=40; INPUT_SEEDS=30
else
	BYTE_CUTS="1 2 3 5 8 13 21 34 55"; PCT_CUTS="2 5 10 25 40 60 80"; FLIP_SEEDS=200; INPUT_SEEDS=150
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

# Frame count: big-endian at offset 7. Not a guess -- the value was read back
# out of a real archive to confirm offset and byte order, after a first probe
# wrote it little-endian and produced a spurious timeout that looked like a
# hang bug.
cat > "$WORK/hdr.py" <<'HDR'
import sys, struct
d = bytearray(open(sys.argv[1], 'rb').read())
d[7:11] = struct.pack('>I', int(sys.argv[3]) & 0xFFFFFFFF)
open(sys.argv[2], 'wb').write(d)
HDR

# Corruption for the INPUT side. Aimed at the bytes just after each frame
# sync word, which is where the side info lives -- random bytes anywhere else
# mostly land in payload the parser never interprets.
cat > "$WORK/mflip.py" <<'MPY'
import sys, random
src, dst, seed, k = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
d = bytearray(open(src, 'rb').read())
r = random.Random(seed)
pos = [i for i in range(len(d)-1) if d[i] == 0xFF and (d[i+1] & 0xE0) == 0xE0]
for _ in range(k):
    if pos and r.random() < 0.8:
        p = r.choice(pos); off = p + r.randint(2, 34)
        if off < len(d): d[off] = r.randrange(256)
    else:
        d[r.randrange(len(d))] = r.randrange(256)
open(dst, 'wb').write(d)
MPY

flip_file() { # $1 archive $2 out $3 seed $4 nflips
	python3 "$WORK/flip.py" "$1" "$2" "$3" "$4" || return 1
	[ -s "$2" ] || return 1
}

# Classify one damaged archive. Echoes: crash | hang | reject | wrong | ident
#
# Also records the elapsed time in g_ms, because the verdict alone is a binary
# read of a continuous variable. A cell that takes thirty seconds and produces
# output is classified identically to one that takes ninety milliseconds and
# produces output -- and whether the slow one shows up as a hang at all is
# decided by where the timeout happens to sit, not by anything about the input.
# @PJPG measured a case at 25.7 s against a 431 ms baseline that produced
# output and would have been one more "wrong" row at a 30 s timeout. Their
# whole 7920-cell grid could not have found it: not because the cells miss it,
# but because the instrument had no such axis.
classify() { # $1 binary $2 damaged archive $3 sha of the original mp3
	local bin=$1 arch=$2 ref=$3 out rc t0 t1 g_ms
	rm -rf "$WORK/out"; mkdir -p "$WORK/out"
	t0=$( date +%s%N )
	# BOTH streams captured: msgout is stdout by default, and a harness that
	# greps only stderr sees every rejection as silence.
	timeout "$TIMEOUT" "$bin" x -o -np -od"$WORK/out" "$arch" >/dev/null 2>&1
	rc=$?
	t1=$( date +%s%N )
	g_ms=$(( ( t1 - t0 ) / 1000000 ))
	# Verdict AND duration on one line: classify runs inside $( ), which is a
	# subshell, so anything it assigns to a global is lost. The first version
	# set g_ms and every timing number came out zero -- a whole axis reading as
	# "nothing is ever slow" because of the shell, not the code under test.
	[ $rc -ge 128 ] && { echo "crash $g_ms"; return; }
	[ $rc -eq 124 ] && { echo "hang $g_ms"; return; }
	out=$( ls "$WORK/out" 2>/dev/null | head -1 )
	[ -n "$out" ] && [ -s "$WORK/out/$out" ] || { echo "reject $g_ms"; return; }
	if [ "$( sha256sum "$WORK/out/$out" | cut -d' ' -f1 )" = "$ref" ]; then
		echo "ident $g_ms"
	else
		echo "wrong $g_ms"
	fi
}

# Median duration of a healthy decode, so slowness is measured against this
# build on this machine rather than a hardcoded number.
baseline_ms() { # $1 binary
	local bin=$1 f arch t0 t1 n=0
	: > "$WORK/base.txt"
	rm -rf "$WORK/ba"; mkdir -p "$WORK/ba/out"
	for f in $SOURCES; do
		"$bin" a -o -np -od"$WORK/ba" "$f" >/dev/null 2>&1
		arch=$( ls "$WORK/ba"/*.pm3 2>/dev/null | head -1 ); [ -n "$arch" ] || continue
		rm -rf "$WORK/ba/out"; mkdir -p "$WORK/ba/out"
		t0=$( date +%s%N )
		timeout "$TIMEOUT" "$bin" x -o -np -od"$WORK/ba/out" "$arch" >/dev/null 2>&1
		t1=$( date +%s%N )
		echo $(( ( t1 - t0 ) / 1000000 )) >> "$WORK/base.txt"
		rm -f "$WORK/ba"/*.pm3; n=$(( n + 1 ))
	done
	[ "$n" -gt 0 ] || { echo 100; return; }
	sort -n "$WORK/base.txt" | awk '{a[NR]=$1} END{ print (NR%2) ? a[(NR+1)/2] : int((a[NR/2]+a[NR/2+1])/2) }'
}

# Flag a cell as slow relative to the healthy baseline. Kept separate from the
# verdict on purpose: "slow AND produced output" is the class the verdict hides
# completely, since it reads as an ordinary wrong-output row.
note_time() { # $1 verdict
	[ "$g_ms" -gt "$g_maxms" ] && g_maxms=$g_ms
	{ [ "$g_minms" -lt 0 ] || [ "$g_ms" -lt "$g_minms" ]; } && g_minms=$g_ms

	# BOTH ENDS. The first version tracked only the maximum, which is the
	# one-sided-tracker defect found in a throwaway probe an hour earlier and
	# then, on looking, sitting in this durable harness too: "slowest" alone
	# cannot distinguish a healthy run from a binary that returns instantly
	# without doing the work. Implausibly FAST matters only when the cell also
	# claims to have produced output -- a rejection returns quickly by design,
	# a successful decode in 2ms when a healthy one takes 115ms did not decode
	# anything.
	case $1 in
		wrong|ident)
			[ "$g_ms" -le "$FAST_MS" ] && g_fast_out=$(( g_fast_out + 1 ))
			;;
	esac

	[ "$g_ms" -ge "$SLOW_MS" ] || return 0
	g_slow=$(( g_slow + 1 ))
	case $1 in wrong|ident) g_slow_out=$(( g_slow_out + 1 ));; esac
}

# Run every regime against one binary. Sets the g_* counters.
sweep() { # $1 binary $2 label
	local bin=$1 label=$2 f b arch ref full k pct seed verdict rc carch cback
	g_crash=0; g_hang=0; g_wrong=0; g_cells=0
	g_r1=0; g_r2=0; g_r3=0; g_r4=0; g_r6=0; g_unreadable=0; g_refused=0; g_rt_ok=0; g_r6_sig=0; g_r6_wrong=0
	g_slow=0; g_slow_out=0; g_maxms=0; g_minms=-1; g_fast_out=0; g_canary=""; g_books=""; g_skipped=0
	for f in $SOURCES; do
		b=$( basename "$f" )
		rm -rf "$WORK/a"; mkdir -p "$WORK/a"
		"$bin" a -o -np -od"$WORK/a" "$f" >/dev/null 2>&1
		arch=$( ls "$WORK/a"/*.pm3 2>/dev/null | head -1 )
		[ -n "$arch" ] || { g_skipped=$(( g_skipped + 1 )); continue; }
		ref=$( sha256sum "$f" | cut -d' ' -f1 )
		full=$( stat -c%s "$arch" )
		[ "$full" -gt 200 ] || { g_skipped=$(( g_skipped + 1 )); continue; }

		# CANARY: the UNDAMAGED archive, run through the same path as every
		# damaged one and required to come back identical. Not a test of the
		# codec -- regression.sh covers that -- but of this harness. If the
		# binary is missing, the output directory is wrong, or the hash
		# comparison is broken, every cell reads as "reject" and the run looks
		# like a decoder that rejects everything, which is a coherent and
		# entirely false result. @PJPG lost a whole run to exactly that: a
		# missing /usr/bin/time made every row report "no output", and the row
		# for the intact file is what exposed it. A control you have to
		# remember to run separately is one you will eventually skip; a row
		# inside the same table cannot be skipped.
		#
		# Verified against the failure it exists for, not just against its own
		# reporting path: with the decode writing to one directory and this
		# function reading another -- an ordinary path bug -- the run prints
		#
		#   cells=122  crash=0  hang=0  silently-wrong=0
		#
		# which is the BEST result this suite can produce, and the canary is the
		# only thing that says otherwise. Inverting the canary's own comparison
		# proves the reporting path works; it does not prove the canary catches
		# a broken harness. Those are different claims and the first is much
		# weaker -- @PJPG's fifteenth failure shape, a control that verifies the
		# existence of an effect rather than its cause.
		verdict=$( classify "$bin" "$arch" "$ref" ); verdict=${verdict%% *}
		if [ "$verdict" != "ident" ]; then
			g_canary="$g_canary $( basename "$f" )($verdict)"
		fi

		# REGIME 1: bytes off the end. The one the old sweeps missed.
		for k in $BYTE_CUTS; do
			head -c $(( full - k )) "$arch" > "$WORK/d.pm3"
			verdict=$( classify "$bin" "$WORK/d.pm3" "$ref" ); g_ms=${verdict#* }; verdict=${verdict%% *}; g_cells=$(( g_cells + 1 )); g_r1=$(( g_r1 + 1 ))
			case $verdict in crash) g_crash=$(( g_crash + 1 ));; hang) g_hang=$(( g_hang + 1 ));;
				wrong) g_wrong=$(( g_wrong + 1 ));; esac
			note_time "$verdict"
		done

		# REGIME 2: percentage of the file.
		for pct in $PCT_CUTS; do
			head -c $(( full * pct / 100 )) "$arch" > "$WORK/d.pm3"
			verdict=$( classify "$bin" "$WORK/d.pm3" "$ref" ); g_ms=${verdict#* }; verdict=${verdict%% *}; g_cells=$(( g_cells + 1 )); g_r2=$(( g_r2 + 1 ))
			case $verdict in crash) g_crash=$(( g_crash + 1 ));; hang) g_hang=$(( g_hang + 1 ));;
				wrong) g_wrong=$(( g_wrong + 1 ));; esac
			note_time "$verdict"
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
			flip_file "$arch" "$WORK/d.pm3" "$seed" 1 || { g_skipped=$(( g_skipped + 1 )); continue; }
			verdict=$( classify "$bin" "$WORK/d.pm3" "$ref" ); g_ms=${verdict#* }; verdict=${verdict%% *}; g_cells=$(( g_cells + 1 )); g_r3=$(( g_r3 + 1 ))
			case $verdict in crash) g_crash=$(( g_crash + 1 ));; hang) g_hang=$(( g_hang + 1 ));;
				wrong) g_wrong=$(( g_wrong + 1 ));; esac
			note_time "$verdict"
			flip_file "$arch" "$WORK/d.pm3" $(( seed + 100000 )) 3 || { g_skipped=$(( g_skipped + 1 )); continue; }
			verdict=$( classify "$bin" "$WORK/d.pm3" "$ref" ); g_ms=${verdict#* }; verdict=${verdict%% *}; g_cells=$(( g_cells + 1 )); g_r4=$(( g_r4 + 1 ))
			case $verdict in crash) g_crash=$(( g_crash + 1 ));; hang) g_hang=$(( g_hang + 1 ));;
				wrong) g_wrong=$(( g_wrong + 1 ));; esac
			note_time "$verdict"
		done
	fi
	# REGIME 6: the INPUT side. Every regime above damages the .pm3 and
	# decodes it; none feeds a damaged .mp3 to the COMPRESSOR. That is the path
	# where the most reachable of this week's overflows lived -- the ancillary
	# write is hit by anyone compressing a file they did not produce -- and the
	# harness had no cell on it at all. packMP3 is lossless, so the oracle is
	# exact: whatever compresses must decompress back to the same bytes,
	# corrupt input included. Prompted by @LPJPG, who built the equivalent for
	# packJPG after finding the in-tree fuzzer only ever attacked the archive
	# side.
	set -- $SOURCES
	for seed in $( seq 1 $INPUT_SEEDS ); do
		python3 "$WORK/mflip.py" "$1" "$WORK/c.mp3" "$seed" 12 2>/dev/null || { g_skipped=$(( g_skipped + 1 )); continue; }
		[ -s "$WORK/c.mp3" ] || { g_skipped=$(( g_skipped + 1 )); continue; }
		rm -rf "$WORK/ci"; mkdir -p "$WORK/ci/back"
		g_cells=$(( g_cells + 1 )); g_r6=$(( g_r6 + 1 ))
		timeout "$TIMEOUT" "$bin" a -o -np -od"$WORK/ci" "$WORK/c.mp3" >/dev/null 2>&1
		rc=$?
		if [ $rc -ge 128 ]; then g_crash=$(( g_crash + 1 )); g_r6_sig=$(( g_r6_sig + 1 )); continue; fi
		[ $rc -eq 124 ] && { g_hang=$(( g_hang + 1 )); g_r6_sig=$(( g_r6_sig + 1 )); continue; }
		carch=$( ls "$WORK/ci"/*.pm3 2>/dev/null | head -1 )
		# Refused at compress: not a bug, but it still needs a bucket. An
		# outcome with no counter is invisible, and the accounting check below
		# is what makes that impossible rather than merely remembered.
		[ -n "$carch" ] || { g_refused=$(( g_refused + 1 )); continue; }
		timeout "$TIMEOUT" "$bin" x -o -np -od"$WORK/ci/back" "$carch" >/dev/null 2>&1
		rc=$?
		if [ $rc -ge 128 ]; then g_crash=$(( g_crash + 1 )); g_r6_sig=$(( g_r6_sig + 1 )); continue; fi
		[ $rc -eq 124 ] && { g_hang=$(( g_hang + 1 )); g_r6_sig=$(( g_r6_sig + 1 )); continue; }
		cback=$( ls "$WORK/ci/back" 2>/dev/null | head -1 )
		# Two different failures, kept apart. "Compressed, then the archive was
		# refused" breaks the lossless contract but loses nothing silently --
		# the user is told. "Decoded to different bytes" is silent corruption.
		# Counting them together would hide an improvement inside a regression:
		# both of the cases seen here CRASHED v3.0e and are clean rejections
		# now, so the same cell moved from the worst class to a mild one while
		# a single counter would have read it as two new silent failures.
		if [ -z "$cback" ]; then
			g_unreadable=$(( g_unreadable + 1 ))
		elif [ "$( sha256sum "$WORK/c.mp3" | cut -d" " -f1 )" != \
		       "$( sha256sum "$WORK/ci/back/$cback" | cut -d" " -f1 )" ]; then
			g_wrong=$(( g_wrong + 1 )); g_r6_wrong=$(( g_r6_wrong + 1 ))
		else
			# The success case needs a counter too. Two increments were missing
			# here -- this one and the regime-local wrong count -- because two
			# edits meant to add them matched nothing and said so to nobody.
			# Thirteen of thirty cells landed in no bucket at all, and the
			# accounting check below caught it on its first run, in the code
			# written to implement the accounting check.
			g_rt_ok=$(( g_rt_ok + 1 ))
		fi
	done

	printf "  %-22s cells=%-5d crash=%-4d hang=%-4d silently-wrong=%d\n" \
		"$label" "$g_cells" "$g_crash" "$g_hang" "$g_wrong"
	printf "  %-22s per regime: bytes=%d percent=%d 1-flip=%d 3-flip=%d\n" \
		"" "$g_r1" "$g_r2" "$g_r3" "$g_r4"
	printf "  %-22s input side: %d cells = %d refused + %d round-trip ok + %d unreadable + %d wrong + %d signalled\n" \
		"" "$g_r6" "$g_refused" "$g_rt_ok" "$g_unreadable" "$g_r6_wrong" "$g_r6_sig"
	# SECOND INVARIANT: cells that vanish from the DENOMINATOR rather than
	# landing in the wrong bucket. Every skip above -- a source that will not
	# compress, a generator that fails -- used to `continue` before the cell
	# was counted, so the cell left the table entirely and the denominator
	# shrank on its own. A rate over a silently smaller denominator is the
	# same failure one step further out: not a class absorbed by its
	# neighbour, a class that walks off the table. @PJPG named it and checked
	# their own sweeps by hand; this makes it impossible to miss instead.
	[ "$g_skipped" -gt 0 ] && g_books="$g_books  $g_skipped cell(s) skipped before being counted"

	# ACCOUNTING. Every cell must land in exactly one bucket, and the buckets
	# must sum to the cell count. This is the mechanism for the thing that bit
	# me an hour ago: a class that did not exist when the classifier was
	# written gets absorbed into a neighbouring bucket, or falls through with
	# no bucket at all, and nothing says so. @PJPG's formulation is the one
	# this implements -- separating classes is not a decision taken once, it
	# is retaken every time a new class appears, and the new class almost
	# always appears AFTER the classifier. A sum that no longer adds up is the
	# only thing that notices without anyone remembering to look.
	g_acct=$(( g_refused + g_rt_ok + g_unreadable + g_r6_wrong + g_r6_sig ))
	[ "$g_acct" -eq "$g_r6" ] || g_books=" input-side: $g_r6 cells but $g_acct accounted for"
	[ -n "$g_canary" ] && printf "  %-22s CANARY FAILED:%s\n" "" "$g_canary"
	[ "$g_skipped" -gt 0 ] && printf "  %-22s SKIPPED before counting: %d\n" "" "$g_skipped"
	printf "  %-22s timing: baseline=%dms  slow>%dms: %d (with output %d)  fast<%dms with output: %d  [%d..%dms]\n" \
		"" "$BASE_MS" "$SLOW_MS" "$g_slow" "$g_slow_out" "$FAST_MS" "$g_fast_out" "$g_minms" "$g_maxms"
	# An empty regime is a broken harness reporting a clean pass. This is not
	# hypothetical: the first version of this script mangled the flip
	# generator and sailed through both flip regimes without running a case.
	g_empty=""
	[ "$g_r1" -eq 0 ] && g_empty="$g_empty byte-cut"
	[ "$g_r2" -eq 0 ] && g_empty="$g_empty percent-cut"
	[ "$g_r3" -eq 0 ] && g_empty="$g_empty single-flip"
	[ "$g_r4" -eq 0 ] && g_empty="$g_empty multi-flip"
	[ "$g_r6" -eq 0 ] && g_empty="$g_empty input-side"
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
#   source:seed:flips   guard
#   plain:2:1           coefficient magnitude past the Huffman table maximum
#   plain:10:1          ancillary size outside the LAME prediction buffer
#   plain:25:1          arithmetic decoder exhausted while reading main data
#   plain:114:3         sv_bound outside the granule  <- rare, 3 in 1000
#   plain:139:1         region sizes indexing past bandwidth_bounds[23]
#   plain:364:1         Huffman table index with no model behind it
#   plain:1:1           LAME ancillary prediction mismatch
#   plain:53:1          central refusal in uncompress_pmp
#   cover_jpg:2:1       meta-data block
#   cover_jpg:43:1      APIC record
#   cover_jpg:104:1     packJPG cover decode
#
# The last four came from asking the completing half of the question: not
# "does the grid cover this bug" but "which reachable rejection sites have no
# case of their own". Tagging every rejection site with its line number showed
# the grid reaches 12 of 69, and that five of those twelve were unpinned.
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
	# Cover-art guards need an archive that HAS cover art, so the source file
	# is part of each case rather than fixed.
	rm -rf "$WORK/ac"; mkdir -p "$WORK/ac"
	if [ -f "$DATA/cover_jpg.mp3" ]; then
		"$bin" a -o -np -od"$WORK/ac" "$DATA/cover_jpg.mp3" >/dev/null 2>&1
	fi
	printf '%s\n' \
		"plain:2:1:coefficients" \
		"plain:10:1:ancillary size" \
		"plain:25:1:main data" \
		"plain:114:3:sv_bound" \
		"plain:139:1:region sizes" \
		"plain:364:1:bad huffman table" \
		"plain:1:1:ancilary prediction error" \
		"plain:53:1:arithmetic decoder" \
		"cover_jpg:2:1:meta-data" \
		"cover_jpg:43:1:corrupt APIC record" \
		"cover_jpg:104:1:packJPG decode failed" | while IFS= read -r spec; do
		src=${spec%%:*}; spec=${spec#*:}
		seed=${spec%%:*}; spec=${spec#*:}; k=${spec%%:*}; want=${spec#*:}
		case $src in
			plain)     from="$arch";;
			cover_jpg) from="$WORK/ac/cover_jpg.pm3";;
		esac
		[ -s "$from" ] || { echo "$want (no $src archive)"; continue; }
		flip_file "$from" "$WORK/g.pm3" "$seed" "$k" || { echo "$want"; continue; }
		rm -rf "$WORK/out"; mkdir -p "$WORK/out"
		# stdout AND stderr: msgout is stdout by default.
		out=$( timeout 60 "$bin" x -o -np -od"$WORK/out" "$WORK/g.pm3" 2>&1 )
		case $out in *"$want"*) ;; *) echo "$want";; esac
		# UPSTREAM POSITION. Rejecting is not enough: a guard placed AFTER the
		# access it protects still rejects, and the verdict looks identical.
		# @LPJPG materialised that inside their own fix -- rc=1 AND a sanitizer
		# finding on the same run, because the if-condition dereferenced before
		# the guard inside it could act. The two coexist exactly when the guard
		# is downstream, so a sanitizer build turns "rejects" into "rejects and
		# is in the right place". Silent on a normal build, which has no such
		# text to find.
		case $out in
			*AddressSanitizer*|*"runtime error"*) echo "$want (guard is DOWNSTREAM of the access)";;
		esac
	done > "$WORK/missing.txt"

	# The two header guards are not reachable by flipping bits in a stream: the
	# frame count sits at a fixed offset and a random flip almost never lands a
	# value that is both wrong and interesting. Crafted directly instead. Both
	# of these crashed every build up to and including v3.0f, and both are
	# reachable with a hand-made file rather than a damaged one.
	for v in 0 4294967295; do
		python3 "$WORK/hdr.py" "$arch" "$WORK/h.pm3" "$v" 2>/dev/null || {
			echo "frame count (generator failed)" >> "$WORK/missing.txt"; continue; }
		rm -rf "$WORK/out"; mkdir -p "$WORK/out"
		out=$( timeout 60 "$bin" x -o -np -od"$WORK/out" "$WORK/h.pm3" 2>&1 )
		case $out in *"frame count"*) ;; *) echo "frame count nframes=$v" >> "$WORK/missing.txt";; esac
		case $out in
			*AddressSanitizer*|*"runtime error"*)
				echo "frame count nframes=$v (guard is DOWNSTREAM)" >> "$WORK/missing.txt";;
		esac
	done

	g_missing=$( tr '\n' ',' < "$WORK/missing.txt" | sed 's/,$//; s/,/, /g' )
}

echo "packMP3 corruption suite -- six regimes, declared here rather than chosen per run"
echo
BASE_MS=$( baseline_ms "$BIN" )
SLOW_MS=$(( BASE_MS * SLOW_FACTOR ))
FAST_MS=$(( BASE_MS / FAST_DIV ))
[ "$SLOW_MS" -gt 0 ] || SLOW_MS=1
sweep "$BIN" "$( basename "$BIN" )"
new_crash=$g_crash; new_hang=$g_hang; new_wrong=$g_wrong; new_empty=$g_empty
new_slow=$g_slow; new_canary=$g_canary; new_fast=$g_fast_out; new_unread=$g_unreadable; new_books=$g_books
guards "$BIN"
if [ -z "$g_missing" ]; then
	echo "                         every guard fired on its own case (13/13: 11 seeded + 2 crafted headers)"
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
# Slowness is a failure, not a note. Nothing in this grid comes close today --
# the slowest corrupted cell runs at 1.3x a healthy decode against a 10x
# threshold -- so any cell crossing it is new behaviour and worth stopping for.
if [ -n "$new_canary" ]; then
	echo "  FAIL: undamaged archive(s) did not round-trip:$new_canary"
	echo "        The harness is broken, not the codec -- every other number in"
	echo "        this run is void. Check the binary, the paths, and the hashing."
	fail=1
fi
if [ -n "$new_books" ]; then
	echo "  FAIL: outcome classes do not add up --$new_books"
	echo "        A cell landed in no bucket, or in two. Some outcome exists that"
	echo "        the classifier was not written for."
	fail=1
fi
if [ "$new_unread" -gt "$UNREADABLE_BASELINE" ]; then
	echo "  FAIL: compressed-then-unreadable rose to $new_unread (baseline $UNREADABLE_BASELINE)"
	echo "        The compressor accepted a file and the decompressor refused its own"
	echo "        output. Not silent -- the user is told -- but the round trip is gone."
	fail=1
fi
if [ "$new_fast" -gt 0 ]; then
	echo "  FAIL: $new_fast cell(s) produced output in under ${FAST_MS}ms (baseline ${BASE_MS}ms)"
	echo "        A decode that claims success far faster than a healthy one did not decode."
	fail=1
fi
if [ "$new_slow" -gt 0 ]; then
	echo "  FAIL: $new_slow cell(s) took over ${SLOW_MS}ms (${SLOW_FACTOR}x the ${BASE_MS}ms baseline)"
	echo "        Of those, $g_slow_out produced output -- that subset is invisible to the"
	echo "        verdict, which reads them as ordinary wrong-output rows."
	fail=1
fi
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
