#!/bin/sh
#
# check-il-offsets.sh - assert tier-1 frames report the same IL offsets as the
# classic JIT.
#
# Tier 1 recovers its native_offset -> il_offset map by reading back stackmap
# markers from the emitted object (recover_il_seq_points (), translator.cpp).
# Nothing about execution goes wrong when that map is off, so a corpus run stays
# green while stack traces quietly blame the wrong IL - or the wrong method,
# since markers from a body inlined into the root land in the root's section
# indistinguishable from its own.
#
# The classic JIT is the oracle: it computes the same mapping during its own
# codegen, so this compares the two rather than hardcoding offsets, which would
# only re-encode whatever the C# compiler happened to emit.
#
# Tier 1 legitimately has FEWER frames - it inlines more - so the rule is:
#
#   the tier-1 frames must be a subsequence of the classic frames, and every
#   frame present in both must report the same IL offset.
#
# A missing frame is an inline; a frame whose offset moved is a mapping bug. In
# particular a caller that inlined a callee must still report its own call site,
# which is what the classic run has for that frame.
#
# Usage: check-il-offsets.sh <runtime> <corpus.exe>
# where <runtime> is a single word or a quoted command prefix.

set -u

if [ $# -lt 2 ]; then
	echo "usage: $0 <runtime> <corpus.exe>" >&2
	exit 2
fi

RUNTIME="$1"
CORPUS="$2"

CLASSIC=$(mktemp -t il-offsets-classic.XXXXXX) || exit 2
TIERED=$(mktemp -t il-offsets-tiered.XXXXXX) || exit 2
trap 'rm -f "$CLASSIC" "$TIERED"' EXIT

# $RUNTIME arrives as a command prefix with its own environment assignments in it
# ($(MINI_RUNTIME) is "MONO_PATH=... /path/to/mono ..."), so it has to go through
# the shell rather than being run as one word.
eval "$RUNTIME --nollvm \"\$CORPUS\"" >"$CLASSIC" 2>/dev/null
rc=$?
if [ $rc -ne 0 ]; then
	echo "check-il-offsets: the classic run failed (exit $rc)" >&2
	exit 1
fi

# Threshold 0 promotes at the tier-0 publish site instead of handing the compile
# to a background worker, so every method is tier 1 by the time it runs and the
# trace does not depend on what a worker finished first.
MONO_TIERED=1
MONO_TIERED_CALL_THRESHOLD=0
export MONO_TIERED MONO_TIERED_CALL_THRESHOLD

eval "$RUNTIME --llvm \"\$CORPUS\"" >"$TIERED" 2>/dev/null
rc=$?
if [ $rc -ne 0 ]; then
	echo "check-il-offsets: the tier-1 run failed (exit $rc)" >&2
	exit 1
fi

if [ ! -s "$CLASSIC" ]; then
	echo "check-il-offsets: the classic run produced no frames at all." >&2
	exit 1
fi

awk -v classic="$CLASSIC" '
	# Both files are "<label>\t<Class:Method>\t<0xIL>" lines, in the order the
	# frames were walked. Compare per label, so an extra or missing scenario is
	# caught as such instead of silently shifting every later comparison.
	function fail(msg) { printf "  FAIL %s\n", msg; bad++ }

	BEGIN {
		while ((getline line < classic) > 0) {
			n = split (line, f, "\t")
			if (n < 3)
				continue
			lbl = f[1]
			cn[lbl]++
			cmeth[lbl, cn[lbl]] = f[2]
			coff[lbl, cn[lbl]] = f[3]
			if (!(lbl in seenlbl)) { seenlbl[lbl] = 1; order[++nlbl] = lbl }
		}
	}

	{
		n = split ($0, f, "\t")
		if (n < 3)
			next
		lbl = f[1]
		tn[lbl]++
		tmeth[lbl, tn[lbl]] = f[2]
		toff[lbl, tn[lbl]] = f[3]
	}

	END {
		if (nlbl == 0) {
			print "check-il-offsets: the classic run produced no labelled frames."
			exit 1
		}

		for (i = 1; i <= nlbl; i++) {
			lbl = order[i]
			if (!(lbl in tn)) {
				fail(lbl ": tier 1 produced no frames for this scenario")
				continue
			}

			# Walk the tier-1 frames against the classic ones, allowing classic
			# frames to be skipped (those were inlined away at tier 1) but never
			# the other way round.
			ci = 1
			matched = 0
			inlined = 0
			bad_before = bad + 0
			for (ti = 1; ti <= tn[lbl]; ti++) {
				while (ci <= cn[lbl] && cmeth[lbl, ci] != tmeth[lbl, ti]) {
					ci++
					inlined++
				}
				if (ci > cn[lbl]) {
					fail(lbl ": tier-1 frame " tmeth[lbl, ti] " is not in the classic trace (or is out of order)")
					matched = -1
					break
				}
				if (coff[lbl, ci] != toff[lbl, ti]) {
					fail(lbl ": " tmeth[lbl, ti] " il " toff[lbl, ti] ", classic has " coff[lbl, ci])
				} else {
					matched++
				}
				ci++
			}

			if (matched >= 0 && bad + 0 == bad_before)
				printf "  ok   %-22s %d frame(s) matched, %d inlined away\n", lbl, matched, inlined
			total++
		}

		printf "%d scenarios, %d mismatch(es)\n", total + 0, bad + 0
		exit (bad > 0)
	}
' "$TIERED"
