#!/usr/bin/env python3
"""Assert a managed stack trace blames each frame on the source line it came from.

A JIT'd method's native_offset -> il_offset map is not built during codegen: it
is read back out of the DWARF line table LLVM emitted for the body
(parse_il_line_table, mono/llvm/jit.cpp), where each IL offset rode along as a
line number.  Nothing about executing the method depends on that map being
right, so a corpus stays green while its stack traces blame the wrong IL -- and
an optimizer that sinks a throw, or cross-jumps two of them together, is exactly
what makes the map disagree with the IL it claims to describe.

Each fixture prints one line per managed frame,

    <label>\t<Class:Method>\t<source>:<line>

with the source and line resolved from the frame's IL offset through the
corpus's portable PDB, and carries the trace it should produce as markers in its
own source:

    // IL-FRAME: <label>[,<label>...] <index> <Class:Method>

sitting on the line the frame has to be blamed on.  The expected line number is
therefore wherever the comment is, so moving code around inside a fixture cannot
leave a stale expectation behind; INDEX is the frame's position in that label's
trace, innermost first.

The rule is equality: the frames reported for a label are exactly the ones
marked for it, in order, each at its own marker's line.  A line is a coarser
claim than an IL offset, but it is one a reader can check against the fixture --
"this frame is blamed on the throw" -- where a hardcoded offset would only
re-encode whatever the C# compiler happened to emit.
"""

import argparse
import collections
import os
import re
import subprocess
import sys

MARKER = re.compile(r"//\s*IL-FRAME:\s*(\S+)\s+(\d+)\s+(\S+)\s*$")


def die(message, *details):
    """Report a problem with the run itself, rather than with what it reported."""
    print(f"{os.path.basename(sys.argv[0])}: {message}", file=sys.stderr)
    for detail in details:
        print(f"  {detail}", file=sys.stderr)
    sys.exit(1)


def expectations(source):
    """Read the fixture's markers into label -> [(method, line)], innermost first."""
    marked = collections.defaultdict(dict)
    with open(source, encoding="utf-8") as handle:
        for lineno, text in enumerate(handle, start=1):
            found = MARKER.search(text.rstrip())
            if not found:
                continue
            labels, index, method = found.groups()
            for label in labels.split(","):
                slot = marked[label]
                if int(index) in slot:
                    die(f"{label} frame {index} is marked twice, at line {lineno}")
                slot[int(index)] = (method, lineno)

    want = {}
    for label, slot in marked.items():
        missing = set(range(len(slot))) - set(slot)
        if missing:
            die(f"{label} has no marker for frame {min(missing)}")
        want[label] = [slot[i] for i in sorted(slot)]
    return want


def reported(output, source_name):
    """Group the corpus's `<label>\\t<method>\\t<file>:<line>` rows by label."""
    walked = collections.defaultdict(list)
    for row in output.splitlines():
        fields = row.split("\t")
        if len(fields) != 3:
            continue
        label, method, where = fields
        name, _, line = where.rpartition(":")
        if name != source_name:
            die(f"{label}: frame {method} came back from {where!r}",
                "The runtime resolved no source line for it, so either the",
                f"portable PDB for the corpus is missing or {source_name} moved.")
        walked[label].append((method, int(line)))
    return walked


def compare(label, want, got):
    """Diff one scenario's frames, returning a message per mismatch."""
    issues = []
    for i, ((wm, wl), (gm, gl)) in enumerate(zip(want, got)):
        if wm != gm:
            issues.append(f"{label} frame {i}: got {gm}, marked {wm}")
        elif wl != gl:
            issues.append(f"{label} frame {i}: {gm} blamed on line {gl}, marked {wl}")
    for i, (method, line) in enumerate(want[len(got):], start=len(got)):
        issues.append(f"{label} frame {i}: {method} (line {line}) never reported")
    for i, (method, line) in enumerate(got[len(want):], start=len(want)):
        issues.append(f"{label} frame {i}: unmarked {method} (line {line})")
    return issues


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runtime", help="the mono binary, or the mono-wrapper script")
    parser.add_argument("corpus", help="the corpus .exe to run")
    parser.add_argument("source", help="the corpus source carrying the markers")
    args = parser.parse_args()

    if not os.path.isfile(args.source):
        die(f"no such source: {args.source}")
    want = expectations(args.source)
    if not want:
        die(f"{args.source} carries no IL-FRAME markers, so there is nothing to check.")

    # --debug is what makes the runtime load the corpus's PDB and print a
    # source line; it leaves the compile itself alone, so the map under test is
    # the one an ordinary run would get.
    proc = subprocess.run([args.runtime, "--debug", args.corpus],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          universal_newlines=True)
    if proc.returncode != 0:
        die(f"the corpus run failed (exit {proc.returncode})",
            *proc.stderr.splitlines()[-20:])
    got = reported(proc.stdout, os.path.basename(args.source))

    checks = problems = 0
    for label in sorted(set(want) | set(got)):
        checks += 1
        issues = compare(label, want.get(label, []), got.get(label, []))
        if not issues:
            print(f"  ok   {label:<24} {len(want[label])} frame(s)")
            continue
        problems += 1
        for issue in issues:
            print(f"  FAIL {issue}")

    print(f"{checks} scenarios, {problems} failed")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
