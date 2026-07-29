#!/usr/bin/env python3
"""Assert tier-1 stack traces name the same methods, at the same IL offsets, as
the classic JIT.

Tier 1 gets its native_offset -> il_offset map, and the chain of bodies inlined
at each offset, out of the debug info LLVM emits for the compiled body.  Nothing
about execution goes wrong when either is off, so a corpus run stays green while
stack traces quietly blame the wrong IL -- or drop a frame, since a body LLVM
folded in has no call site left to name it.

The classic JIT is the oracle: it computes the same mapping during its own
codegen, so this compares the two rather than hardcoding offsets, which would
only re-encode whatever the C# compiler happened to emit.

The rule is equality -- same methods, same order, same IL offsets.  Tier 1
inlines far more than the classic JIT does, so the frames it reports for a
folded-in body are synthesized from that debug info; getting them back is
exactly what is under test, and anything less than equality would pass whether
or not they came back.

That holds only while no fixture method is short enough for the CLASSIC JIT's
own front-end inliner to fold away (INLINE_LENGTH_LIMIT, 20 IL bytes,
method-to-ir.c).  A front-end inline leaves nothing to recover -- the inlined IR
carries the CALLER's offset by construction (cfg->real_offset = inline_offset)
-- so such a method is simply missing from the oracle, while tier 1, which does
no front-end inlining at all, still reports it.  The fixtures are padded well
past that to stay clear of it.
"""

import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import jitcheck


def frames(output):
    """Group `<label>\\t<Class:Method>\\t<0xIL>` rows by label, in walk order."""
    walked = collections.defaultdict(list)
    for line in output.splitlines():
        fields = line.split("\t")
        if len(fields) < 3:
            continue
        walked[fields[0]].append((fields[1], fields[2]))
    return walked


def compare(label, want, got):
    """Diff one scenario's frames, returning a message per mismatch."""
    if got is None:
        return [f"{label}: tier 1 produced no frames for this scenario"]

    issues = []
    # Compare as far as both go, then report any length difference, so that a
    # dropped frame names the frame rather than only the count.
    for i, ((wm, wo), (gm, go)) in enumerate(zip(want, got), start=1):
        if wm != gm:
            issues.append(f"{label} frame {i}: tier 1 has {gm}, classic has {wm}")
        elif wo != go:
            issues.append(f"{label} frame {i}: {gm} il {go}, classic has {wo}")
    for method, offset in want[len(got):]:
        issues.append(f"{label}: tier 1 is missing {method} (il {offset})")
    for method, offset in got[len(want):]:
        issues.append(f"{label}: tier 1 has an extra frame {method} (il {offset})")
    return issues


def main():
    args = jitcheck.argument_parser(__doc__).parse_args()

    proc = jitcheck.run(args.runtime, args.corpus, args=("--nollvm",))
    if proc.returncode != 0:
        jitcheck.die(f"the classic run failed (exit {proc.returncode})")
    classic = frames(proc.stdout)

    proc = jitcheck.run(args.runtime, args.corpus, args=("--llvm",),
                        env=jitcheck.DETERMINISTIC_TIER1)
    if proc.returncode != 0:
        jitcheck.die(f"the tier-1 run failed (exit {proc.returncode})")
    tiered = frames(proc.stdout)

    if not classic:
        jitcheck.die("the classic run produced no labelled frames.")

    report = jitcheck.Report()
    for label, want in classic.items():
        issues = compare(label, want, tiered.get(label))
        if issues:
            report.fail(issues[0])
            for issue in issues[1:]:
                report.problem(issue)
        else:
            report.ok(f"{label:<22} {len(want)} frame(s)")

    # A scenario only the tier-1 run produced would otherwise go unnoticed: the
    # loop above is driven by the classic run.
    for label in tiered:
        if label not in classic:
            report.problem(
                f"{label}: tier 1 produced frames for a scenario the classic run did not")

    return report.finish("scenarios")


if __name__ == "__main__":
    sys.exit(main())
