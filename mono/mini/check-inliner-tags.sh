#!/bin/sh
#
# check-inliner-tags.sh - assert what the tier-1 inliner did to specific callees.
#
# The corpus on its own only ever checks that tier-1 code computes the right
# answer, which it does whether or not anything was inlined - so a gate that
# quietly stops firing, or an inliner that quietly stands down altogether, looks
# exactly like a green run. This turns the pass's own trace into assertions.
#
# Expectations live next to the fixtures they describe, as comments in the
# corpus source:
#
#   // INLINER-EXPECT: folded   Class:Callee (args)   folded into every caller
#   // INLINER-EXPECT: unfolded Class:Callee (args)   offered, but LLVM passed
#   // INLINER-EXPECT: exposed  Class:Callee (args)   materialized (folded or not)
#   // INLINER-EXPECT: refused  Class:Callee (args)   never materialized, any reason
#
# The method text is exactly what the trace prints, which is mono's full method
# name - copy it out of a MONO_INLINER_TRACE=1 run.
#
# "exposed" says the body was made available to LLVM's inliner; whether the cost
# model then took it is LLVM's call, so prefer "folded" when a fixture is really
# about the callee disappearing, and "exposed" when it is about the pass's gates.
#
# Usage: check-inliner-tags.sh <runtime> <corpus.exe> <source.cs>
# where <runtime> is a single word or a quoted command prefix.

set -u

if [ $# -lt 3 ]; then
	echo "usage: $0 <runtime> <corpus.exe> <source.cs>" >&2
	exit 2
fi

RUNTIME="$1"
CORPUS="$2"
SOURCE="$3"

[ -f "$SOURCE" ] || { echo "check-inliner-tags: no such source: $SOURCE" >&2; exit 2; }

TRACE=$(mktemp -t inliner-tags.XXXXXX) || exit 2
trap 'rm -f "$TRACE"' EXIT

# Threshold 0 is the only deterministic mode: above it promotion is handed to a
# background worker, so which methods reach tier 1 before the process exits (and
# therefore which decisions the trace even contains) varies from run to run.
MONO_TIERED=1
MONO_TIERED_CALL_THRESHOLD=0
MONO_INLINER_TRACE=1
export MONO_TIERED MONO_TIERED_CALL_THRESHOLD MONO_INLINER_TRACE

# $RUNTIME arrives as a command prefix with its own environment assignments in
# it ($(MINI_RUNTIME) is "MONO_PATH=... /path/to/mono ..."), so it has to go
# through the shell rather than being run as one word.
eval "$RUNTIME --llvm \"\$CORPUS\"" >/dev/null 2>"$TRACE"
rc=$?

if [ $rc -ne 0 ]; then
	echo "check-inliner-tags: the corpus run failed (exit $rc)" >&2
	tail -20 "$TRACE" >&2
	exit 1
fi

if ! grep -q '^\[inliner\] ' "$TRACE"; then
	echo "check-inliner-tags: the run produced no inliner trace at all." >&2
	echo "  Either the runtime has no LLVM tier, or the pass never ran." >&2
	exit 1
fi

awk -v source="$SOURCE" '
	# The pass names a decision by a verb and then the method. Everything that
	# is not one of the verbs below is a refusal reason - a free-text phrase -
	# so refusals are matched by their method suffix rather than by reason,
	# which keeps this script out of the business of tracking that wording.
	function verb_of(rest) {
		if (rest ~ /^root /)     return "root"
		if (rest ~ /^expose /)   return "expose"
		if (rest ~ /^folded /)   return "folded"
		if (rest ~ /^unfolded /) return "unfolded"
		return "refuse"
	}
	function tail_of(rest, v) {
		if (v == "refuse") return rest
		return substr (rest, index (rest, " ") + 1)
	}
	function ends_with(s, suffix) {
		return length (s) >= length (suffix) &&
			substr (s, length (s) - length (suffix) + 1) == suffix
	}

	/^\[inliner\] / {
		rest = substr ($0, 11)
		v = verb_of(rest)
		t = tail_of(rest, v)
		if (v == "root")
			next
		if (v == "refuse")
			refused[refused_n++] = t
		else
			seen[v " " t] = 1
		next
	}

	END {
		while ((getline line < source) > 0) {
			where = index (line, "INLINER-EXPECT:")
			if (where == 0)
				continue
			spec = substr (line, where + length ("INLINER-EXPECT:"))
			sub (/^[ \t]+/, "", spec)
			sub (/[ \t]+$/, "", spec)
			if (spec == "")
				continue

			want = spec
			sub (/[ \t].*$/, "", want)              # the verb
			method = substr (spec, length (want) + 1)
			sub (/^[ \t]+/, "", method)

			total++
			ok = 0
			if (want == "refused") {
				for (i = 0; i < refused_n; i++)
					if (ends_with(refused[i], method)) { ok = 1; break }
			} else if (want == "folded" || want == "unfolded") {
				ok = ((want " " method) in seen)
			} else if (want == "exposed") {
				ok = (("expose " method) in seen)
			} else {
				printf "  ?? unknown expectation verb \"%s\"\n", want
				bad++
				continue
			}

			if (ok) {
				printf "  ok   %-8s %s\n", want, method
			} else {
				printf "  FAIL %-8s %s\n", want, method
				bad++
			}
		}

		if (total == 0) {
			print "check-inliner-tags: no INLINER-EXPECT lines found in " source
			exit 1
		}
		printf "%d expectations, %d failed\n", total, bad + 0
		exit (bad > 0)
	}
' "$TRACE"
