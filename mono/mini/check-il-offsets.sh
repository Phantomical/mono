#!/bin/sh
#
# check-il-offsets.sh - assert tier-1 stack traces name the same methods, at the
# same IL offsets, as the classic JIT.
#
# Tier 1 gets its native_offset -> il_offset map, and the chain of bodies inlined
# at each offset, out of the debug info LLVM emits for the compiled body. Nothing
# about execution goes wrong when either is off, so a corpus run stays green while
# stack traces quietly blame the wrong IL - or drop a frame, since a body LLVM
# folded in has no call site left to name it.
#
# The classic JIT is the oracle: it computes the same mapping during its own
# codegen, so this compares the two rather than hardcoding offsets, which would
# only re-encode whatever the C# compiler happened to emit.
#
# The rule is equality - same methods, same order, same IL offsets. Tier 1 inlines
# far more than the classic JIT does, so the frames it reports for a folded-in body
# are synthesized from that debug info; getting them back is exactly what is under
# test, and anything less than equality would pass whether or not they came back.
#
# That holds only while no fixture method has an IL length between the two
# frontends' inline limits (INLINE_LENGTH_LIMIT 20 for the classic JIT,
# LLVM_JIT_INLINE_LENGTH_LIMIT 100 for a tier-1 compile, method-to-ir.c). In that
# window mono's own inliner folds the method away for tier 1 but not for the
# classic JIT, and a frontend inline leaves nothing to recover: the inlined IR
# carries the CALLER's offset by construction (cfg->real_offset = inline_offset),
# so the frame is gone from tier 1 with no debug info describing it. The fixtures
# are padded well past 100 to stay clear of it.
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
			total++

			if (!(lbl in tn)) {
				fail(lbl ": tier 1 produced no frames for this scenario")
				continue
			}

			bad_before = bad + 0

			# Compare as far as both go, then report any length difference, so
			# that a dropped frame names the frame rather than only the count.
			nmin = cn[lbl] < tn[lbl] ? cn[lbl] : tn[lbl]
			for (j = 1; j <= nmin; j++) {
				if (cmeth[lbl, j] != tmeth[lbl, j])
					fail(lbl " frame " j ": tier 1 has " tmeth[lbl, j] ", classic has " cmeth[lbl, j])
				else if (coff[lbl, j] != toff[lbl, j])
					fail(lbl " frame " j ": " tmeth[lbl, j] " il " toff[lbl, j] ", classic has " coff[lbl, j])
			}

			for (j = nmin + 1; j <= cn[lbl]; j++)
				fail(lbl ": tier 1 is missing " cmeth[lbl, j] " (il " coff[lbl, j] ")")
			for (j = nmin + 1; j <= tn[lbl]; j++)
				fail(lbl ": tier 1 has an extra frame " tmeth[lbl, j] " (il " toff[lbl, j] ")")

			if (bad + 0 == bad_before)
				printf "  ok   %-22s %d frame(s)\n", lbl, cn[lbl]
		}

		# A scenario only the tier-1 run produced would otherwise go unnoticed:
		# the loop above is driven by the classic run.
		for (lbl in tn)
			if (!(lbl in seenlbl))
				fail(lbl ": tier 1 produced frames for a scenario the classic run did not")

		printf "%d scenarios, %d mismatch(es)\n", total + 0, bad + 0
		exit (bad > 0)
	}
' "$TIERED"
