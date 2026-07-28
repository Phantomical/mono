#!/bin/sh
#
# check-devirt-tags.sh - assert what the tier-1 devirtualizer did to specific
# call sites.
#
# The corpus on its own only checks that tier-1 code computes the right answer,
# which it does whether or not anything was devirtualized - so a pass that
# quietly stops resolving looks exactly like a green run. This turns the pass's
# own trace into assertions.
#
# Expectations live next to the fixtures they describe, as comments in the
# corpus source:
#
#   // DEVIRT-EXPECT: devirt  Class:Method (args)   resolved to a direct call
#   // DEVIRT-EXPECT: refused Class:Method (args)   left indirect, any reason
#
# The method text is exactly what the trace prints, which is mono's full method
# name - copy it out of a MONO_DEVIRT_TRACE=1 run. Note which method each verb
# names: "devirt" lines name the RESOLVED TARGET, refusals name the DECLARED
# method, because a refused site has no target to name. Refusals are matched by
# method suffix rather than by reason, which keeps this script out of the
# business of tracking that wording.
#
# Usage: check-devirt-tags.sh <runtime> <corpus.exe> <source.cs>
# where <runtime> is a single word or a quoted command prefix.

set -u

if [ $# -lt 3 ]; then
	echo "usage: $0 <runtime> <corpus.exe> <source.cs>" >&2
	exit 2
fi

RUNTIME="$1"
CORPUS="$2"
SOURCE="$3"

[ -f "$SOURCE" ] || { echo "check-devirt-tags: no such source: $SOURCE" >&2; exit 2; }

TRACE=$(mktemp -t devirt-tags.XXXXXX) || exit 2
trap 'rm -f "$TRACE"' EXIT

# Threshold 0 is the only deterministic mode: above it promotion is handed to a
# background worker, so which methods reach tier 1 before the process exits (and
# therefore which decisions the trace even contains) varies from run to run.
MONO_TIERED=1
MONO_TIERED_CALL_THRESHOLD=0
MONO_DEVIRT_TRACE=1
export MONO_TIERED MONO_TIERED_CALL_THRESHOLD MONO_DEVIRT_TRACE

# $RUNTIME arrives as a command prefix with its own environment assignments in
# it ($(MINI_RUNTIME) is "MONO_PATH=... /path/to/mono ..."), so it has to go
# through the shell rather than being run as one word.
eval "$RUNTIME --llvm \"\$CORPUS\"" >/dev/null 2>"$TRACE"
rc=$?

if [ $rc -ne 0 ]; then
	echo "check-devirt-tags: the corpus run failed (exit $rc)" >&2
	tail -20 "$TRACE" >&2
	exit 1
fi

if ! grep -q '^\[devirt\] ' "$TRACE"; then
	echo "check-devirt-tags: the run produced no devirt trace at all." >&2
	echo "  Either the runtime has no LLVM tier, or the pass never ran." >&2
	exit 1
fi

awk -v source="$SOURCE" '
	# "devirt" is the only success verb; everything else the pass prints is a
	# refusal reason, which is free text.
	function ends_with(s, suffix) {
		return length (s) >= length (suffix) &&
			substr (s, length (s) - length (suffix) + 1) == suffix
	}

	/^\[devirt\] / {
		rest = substr ($0, 10)
		# Successes carry the receiver as " on <Class>"; the expectations name
		# the method only, so drop it.
		sub (/ on [^ ]+$/, "", rest)
		if (rest ~ /^devirt /) {
			resolved[substr (rest, index (rest, " ") + 1)] = 1
		} else {
			refused[refused_n++] = substr (rest, index (rest, " ") + 1)
		}
		next
	}

	END {
		while ((getline line < source) > 0) {
			where = index (line, "DEVIRT-EXPECT:")
			if (where == 0)
				continue
			spec = substr (line, where + length ("DEVIRT-EXPECT:"))
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
			if (want == "devirt") {
				ok = (method in resolved)
			} else if (want == "refused") {
				for (i = 0; i < refused_n; i++)
					if (ends_with(refused[i], method)) { ok = 1; break }
				# A site that got resolved is not refused, however many other
				# sites naming the same method were.
				if (method in resolved)
					ok = 0
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
			print "check-devirt-tags: no DEVIRT-EXPECT lines found in " source
			exit 1
		}
		printf "%d expectations, %d failed\n", total, bad + 0
		exit (bad > 0)
	}
' "$TRACE"
