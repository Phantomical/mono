#!/usr/bin/env python3
"""Assert that tier-1 reaches a class's static fields through one shared symbol.

tiered-statics.exe computes the same answers whether each field carries its own
symbol or they all share their class's block, so the corpus alone cannot tell
co-location from the per-field encoding that preceded it -- or from co-location
that quietly stopped working.  This turns MONO_LLVM_CONST_TRACE into assertions
about the encoding actually chosen.

The invariant is structural rather than per-site, so unlike the inliner and
devirt tag checks there are no expectations in the corpus source: every class's
statics share one block, so every class's SFLDA patches must resolve to one
symbol at differing offsets.
"""

import collections
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import jitcheck

BLOCK = "mono_statics_"

# A field outside its class block -- a thread- or context-static -- legitimately
# takes the per-field encoding.  Scoped to the corpus's own classes, because the
# class libraries it drags in do have such fields and they are not what this
# asserts.
PER_FIELD = re.compile(r"^mono_sfld_(Home|Twin|Gen_1|ByRef|Boxes)_")

# Home and Twin have identical layouts, so a block confused between the two
# would still read plausible values.  Gen`1<int> is the case where one symbol
# per class has to mean one per instantiation -- the reference-typed
# instantiations share a gshared one whose statics come from the rgctx rather
# than from an SFLDA patch, which is why only the int one is named.  ByRef is
# reached only by address.
CLASSES = ("Home", "Twin", "ByRef", "Boxes", "Gen_1_System_Int32_")

# Several fields at several offsets is the whole point: a single offset would be
# indistinguishable from a per-field symbol that happened to be named after the
# class.
MIN_OFFSETS = 3


def encodings(trace):
    """Read the SFLDA rows: block symbol -> its distinct offsets, and fallbacks.

    A row is `llvm-const: SFLDA <pad> symbol <name>+<offset>`.
    """
    offsets = collections.defaultdict(set)
    fallbacks = set()
    rows = 0
    for line in trace.splitlines():
        fields = line.split()
        if len(fields) < 4 or fields[1] != "SFLDA" or fields[2] != "symbol":
            continue
        rows += 1
        symbol, _, offset = fields[3].partition("+")
        if not symbol.startswith(BLOCK):
            if PER_FIELD.match(symbol):
                fallbacks.add(symbol)
            continue
        offsets[symbol].add(offset)
    return rows, offsets, fallbacks


def main():
    args = jitcheck.argument_parser(__doc__).parse_args()
    trace = jitcheck.traced_run(args.runtime, args.corpus, "MONO_LLVM_CONST_TRACE")
    rows, offsets, fallbacks = encodings(trace)
    if rows == 0:
        jitcheck.die("the run produced no SFLDA symbols at all.",
                     "Either the runtime has no LLVM tier, or static fields",
                     "stopped being named as symbols.")

    report = jitcheck.Report()
    for cls in CLASSES:
        symbol = BLOCK + cls
        seen = offsets.get(symbol)
        if not seen:
            report.fail(f"{cls}: no static block symbol - its fields were not co-located")
        elif len(seen) < MIN_OFFSETS:
            report.fail(f"{cls}: {len(seen)} distinct offset(s) under {symbol}, "
                        f"expected >= {MIN_OFFSETS}")
        else:
            report.ok(f"{cls}: {len(seen)} fields share {symbol}")

    for symbol in sorted(fallbacks):
        report.fail(f"{symbol}: a static field took the per-field encoding")

    return report.finish("checks")


if __name__ == "__main__":
    sys.exit(main())
