#!/bin/sh
#
# check-statics-symbols.sh - assert that tier-1 reaches a class's static fields
# through one shared symbol rather than one symbol per field.
#
# tiered-statics.exe computes the same answers either way, so the corpus alone
# cannot tell co-location from the per-field encoding that preceded it - or from
# co-location that quietly stopped working. This turns MONO_LLVM_CONST_TRACE
# into assertions about the encoding actually chosen.
#
# The invariant is structural rather than per-site, so unlike check-devirt-tags
# and check-inliner-tags there are no expectations in the corpus source: every
# class's statics share one block, so every class's SFLDA patches must resolve
# to one symbol at differing offsets.
#
# Usage: check-statics-symbols.sh <runtime> <corpus.exe>
# where <runtime> is a single word or a quoted command prefix.

set -u

if [ $# -lt 2 ]; then
	echo "usage: $0 <runtime> <corpus.exe>" >&2
	exit 2
fi

RUNTIME="$1"
CORPUS="$2"

TRACE=$(mktemp -t statics-symbols.XXXXXX) || exit 2
trap 'rm -f "$TRACE"' EXIT

# Threshold 0 promotes on the mutator, so every method the corpus runs is
# compiled at tier 1 before the process exits and the trace is the same on
# every run. Above it promotion is handed to a background worker and which
# decisions the trace contains varies.
MONO_TIERED=1
MONO_TIERED_CALL_THRESHOLD=0
MONO_LLVM_CONST_TRACE=1
export MONO_TIERED MONO_TIERED_CALL_THRESHOLD MONO_LLVM_CONST_TRACE

# $RUNTIME arrives as a command prefix with its own environment assignments in
# it, so it has to go through the shell rather than being run as one word.
eval "$RUNTIME --llvm \"\$CORPUS\"" >/dev/null 2>"$TRACE"
rc=$?

if [ $rc -ne 0 ]; then
	echo "check-statics-symbols: the corpus run failed (exit $rc)" >&2
	tail -20 "$TRACE" >&2
	exit 1
fi

if ! grep -q '^llvm-const: SFLDA .* symbol ' "$TRACE"; then
	echo "check-statics-symbols: the run produced no SFLDA symbols at all." >&2
	echo "  Either the runtime has no LLVM tier, or static fields stopped" >&2
	echo "  being named as symbols." >&2
	exit 1
fi

awk '
	function want_block (cls,   sym) {
		total++
		if (!(cls in symbols)) {
			printf "  MISSING  %s: no static block symbol - its fields were not co-located\n", cls
			bad++
			return
		}
		if (symbols[cls] != 1) {
			printf "  SPLIT    %s: %d symbols, expected one block per class\n", cls, symbols[cls]
			bad++
			return
		}
		sym = "mono_statics_" cls
		# Several fields at several offsets is the whole point: a single
		# offset would be indistinguishable from a per-field symbol that
		# happened to be named after the class.
		if (offsets[sym] < 3) {
			printf "  FLAT     %s: %d distinct offset(s) under %s, expected >= 3\n",
				cls, offsets[sym], sym
			bad++
			return
		}
		printf "  ok       %s: %d fields share %s\n", cls, offsets[sym], sym
	}

	# llvm-const: SFLDA <pad> symbol <name>+<offset>
	$2 == "SFLDA" && $3 == "symbol" {
		split ($4, part, "+")
		sym = part[1]
		off = part[2]

		# Anything not named for a block took the per-field encoding, which
		# only a field outside its class block (a thread- or context-static)
		# should reach. Scoped to the corpus, because the class libraries it
		# drags in do have such fields and they are not what this asserts.
		if (sym !~ /^mono_statics_/) {
			if (sym ~ /^mono_sfld_(Home|Twin|Gen_1|ByRef|Boxes)_/)
				perfield[sym] = 1
			next
		}

		if (!((sym SUBSEP off) in seen)) {
			seen[sym SUBSEP off] = 1
			offsets[sym]++
		}
		# Count symbols per class name, so a class split across two blocks is
		# visible rather than averaged away.
		cls = substr (sym, length ("mono_statics_") + 1)
		if (!((cls SUBSEP sym) in pair)) {
			pair[cls SUBSEP sym] = 1
			symbols[cls]++
		}
	}

	END {
		# Home and Twin have identical layouts, so a block confused between the
		# two would still read plausible values. Gen`1<int> is the case where
		# one symbol per class has to mean one per instantiation - the
		# reference-typed instantiations share a gshared one whose statics come
		# from the rgctx rather than from an SFLDA patch, which is why only the
		# int one is named. ByRef is reached only by address.
		want_block("Home")
		want_block("Twin")
		want_block("ByRef")
		want_block("Boxes")
		want_block("Gen_1_System_Int32_")

		for (p in perfield) {
			printf "  FALLBACK %s: a static field took the per-field encoding\n", p
			bad++
			total++
		}

		printf "check-statics-symbols: %d checks, %d failed\n", total, bad
		exit bad ? 1 : 0
	}
' "$TRACE"
