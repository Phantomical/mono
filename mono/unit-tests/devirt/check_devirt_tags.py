#!/usr/bin/env python3
"""Assert what the tier-1 devirtualizer did to specific call sites.

The corpus on its own only checks that tier-1 code computes the right answer,
which it does whether or not anything was devirtualized -- so a pass that
quietly stops resolving looks exactly like a green run.  This turns the pass's
own trace into assertions.

Expectations live next to the fixtures they describe, as comments in the corpus
source:

    // DEVIRT-EXPECT: devirt  Class:Method (args)   resolved to a direct call
    // DEVIRT-EXPECT: refused Class:Method (args)   left indirect, any reason

The method text is exactly what the trace prints, which is mono's full method
name -- copy it out of a MONO_DEVIRT_TRACE=1 run.  Note which method each verb
names: "devirt" lines name the RESOLVED TARGET, refusals name the DECLARED
method, because a refused site has no target to name.  Refusals are matched by
method suffix rather than by reason, which keeps this check out of the business
of tracking that wording.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import jitcheck

PREFIX = "[devirt] "

# Successes carry the receiver as " on <Class>"; the expectations name the
# method only.
RECEIVER = re.compile(r" on [^ ]+$")


def decisions(lines):
    """Split the trace into the resolved targets and the refusal lines."""
    resolved = set()
    refused = []
    for line in lines:
        rest = RECEIVER.sub("", line[len(PREFIX):])
        verb, _, method = rest.partition(" ")
        if verb == "devirt":
            resolved.add(method)
        else:
            refused.append(method)
    return resolved, refused


def main():
    args = jitcheck.argument_parser(__doc__, source=True).parse_args()
    if not os.path.isfile(args.source):
        jitcheck.die(f"no such source: {args.source}")

    trace = jitcheck.traced_run(args.runtime, args.corpus, "MONO_DEVIRT_TRACE")
    lines = jitcheck.require_trace(
        trace, PREFIX,
        "Either the runtime has no LLVM tier, or the pass never ran.")
    resolved, refused = decisions(lines)

    report = jitcheck.Report()
    for want, method in jitcheck.read_expectations(args.source, "DEVIRT-EXPECT"):
        if want == "devirt":
            ok = method in resolved
        elif want == "refused":
            # A site that got resolved is not refused, however many other sites
            # naming the same method were.
            ok = (any(line.endswith(method) for line in refused)
                  and method not in resolved)
        else:
            report.fail(f"?? unknown expectation verb {want!r}")
            continue
        (report.ok if ok else report.fail)(f"{want:<8} {method}")

    return report.finish("expectations")


if __name__ == "__main__":
    sys.exit(main())
