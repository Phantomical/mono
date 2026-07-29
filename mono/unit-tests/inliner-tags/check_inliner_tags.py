#!/usr/bin/env python3
"""Assert what the tier-1 inliner did to specific callees.

The corpus on its own only ever checks that tier-1 code computes the right
answer, which it does whether or not anything was inlined -- so a gate that
quietly stops firing, or an inliner that quietly stands down altogether, looks
exactly like a green run.  This turns the pass's own trace into assertions.

Expectations live next to the fixtures they describe, as comments in the corpus
source:

    // INLINER-EXPECT: folded   Class:Callee (args)   folded into every caller
    // INLINER-EXPECT: unfolded Class:Callee (args)   offered, but LLVM passed
    // INLINER-EXPECT: exposed  Class:Callee (args)   materialized (folded or not)
    // INLINER-EXPECT: refused  Class:Callee (args)   never materialized, any reason

The method text is exactly what the trace prints, which is mono's full method
name -- copy it out of a MONO_INLINER_TRACE=1 run.

"exposed" says the body was made available to LLVM's inliner; whether the cost
model then took it is LLVM's call, so prefer "folded" when a fixture is really
about the callee disappearing, and "exposed" when it is about the pass's gates.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import jitcheck

PREFIX = "[inliner] "

# The pass names a decision by a verb and then the method.  Anything else it
# prints is a refusal reason -- a free-text phrase -- so refusals are matched by
# their method suffix rather than by reason, which keeps this check out of the
# business of tracking that wording.
VERBS = ("root", "expose", "folded", "unfolded")


def decisions(lines):
    """Split the trace into the (verb, method) pairs and the refusal lines."""
    seen = set()
    refused = []
    for line in lines:
        rest = line[len(PREFIX):]
        verb, _, method = rest.partition(" ")
        if verb == "root":
            continue
        if verb in VERBS:
            seen.add((verb, method))
        else:
            refused.append(rest)
    return seen, refused


def main():
    args = jitcheck.argument_parser(__doc__, source=True).parse_args()
    if not os.path.isfile(args.source):
        jitcheck.die(f"no such source: {args.source}")

    trace = jitcheck.traced_run(args.runtime, args.corpus, "MONO_INLINER_TRACE")
    lines = jitcheck.require_trace(
        trace, PREFIX,
        "Either the runtime has no LLVM tier, or the pass never ran.")
    seen, refused = decisions(lines)

    report = jitcheck.Report()
    for want, method in jitcheck.read_expectations(args.source, "INLINER-EXPECT"):
        if want in ("folded", "unfolded"):
            ok = (want, method) in seen
        elif want == "exposed":
            ok = ("expose", method) in seen
        elif want == "refused":
            ok = any(line.endswith(method) for line in refused)
        else:
            report.fail(f"?? unknown expectation verb {want!r}")
            continue
        (report.ok if ok else report.fail)(f"{want:<8} {method}")

    if report.checks == 0:
        jitcheck.die(f"no INLINER-EXPECT lines found in {args.source}")
    return report.finish("expectations")


if __name__ == "__main__":
    sys.exit(main())
