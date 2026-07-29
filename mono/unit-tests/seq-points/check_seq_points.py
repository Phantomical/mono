#!/usr/bin/env python3
"""Assert that enabling sequence points does not change the code mini emits.

A sequence point is meant to be a marker the JIT records and otherwise steps
over, so turning them on should leave every method byte-for-byte the size it
was.  When an optimization does not know about OP_IL_SEQ_POINT it tends to stop
firing across one, and the method silently grows -- correct code, worse code,
and nothing else notices.  So this compiles the corpus twice and compares.

Only the size of each method is compared, not its bytes: that is enough to catch
an optimization that stood down, and it does not depend on register allocation
being stable.  MONO_DEBUG=single-imm-size is on for both runs because amd64
otherwise picks 32-bit encodings opportunistically, which moves sizes around for
reasons that have nothing to do with sequence points.

This is a tier-0 property -- OP_IL_SEQ_POINT is a mini opcode -- so both runs
are --nollvm.
"""

import difflib
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import jitcheck

EMITTED = re.compile(r"emitted[^()]*")
HEX = re.compile(r"0x[0-9a-fA-F]*")
SIZED = re.compile(r"^Method .*code length (\d+)")
# "Method (wrapper foo) Class:Name (args) ... code length 42" -- the qualifier in
# parentheses is optional, and the name is the token after it.
NAMED = re.compile(r"^Method (?:\([^)]*\) )?(\S+)")


def sizes(runtime, corpus, *, seq_points):
    """The `Method ... code length N` lines from a whole-corpus compile."""
    debug = "single-imm-size" if seq_points else "no-compact-seq-points,single-imm-size"
    proc = jitcheck.run(runtime, corpus, args=("--nollvm", "-v", "--compile-all=1"),
                        env={"MONO_DEBUG": debug})
    if proc.returncode != 0:
        jitcheck.die(f"the compile failed (exit {proc.returncode}) with MONO_DEBUG={debug}",
                     *proc.stderr.splitlines()[-20:])
    # The "emitted at 0x..." part moves with the code cache, so drop it before
    # comparing.
    return sorted(EMITTED.sub("", line) for line in proc.stdout.splitlines()
                  if SIZED.match(line))


def disassembly(runtime, corpus, method, *, seq_points):
    """One method's emitted code, with addresses normalized away."""
    debug = "single-imm-size" if seq_points else "no-compact-seq-points,single-imm-size"
    proc = jitcheck.run(runtime, corpus, args=("--nollvm", "--compile-all=1"),
                        env={"MONO_DEBUG": debug, "MONO_VERBOSE_METHOD": method})
    lines = [HEX.sub("0x0", line) for line in proc.stdout.splitlines()]
    if seq_points:
        lines = [line for line in lines if "il_seq_point" not in line]
    return lines


def main():
    args = jitcheck.argument_parser(__doc__).parse_args()

    without = sizes(args.runtime, args.corpus, seq_points=False)
    with_sp = sizes(args.runtime, args.corpus, seq_points=True)
    if not without:
        jitcheck.die("the compile produced no methods at all.",
                     "'-v --compile-all=1' printed nothing this check recognizes.")

    changed = [line for line in difflib.unified_diff(without, with_sp, n=0)
               if line.startswith("-") and not line.startswith("---")]
    if not changed:
        print(f"{len(without)} methods, none changed by sequence points")
        return 0

    print(f"Detected OP_IL_SEQ_POINT incompatibility on {args.corpus}")
    print(f"  {len(changed)} of {len(without)} methods differ when sequence points are enabled.")
    print("  This is probably caused by a runtime optimization "
          "that is not handling OP_IL_SEQ_POINT")
    print()

    # The smallest differing method is the one worth disassembling: it is the
    # least code to read, and an optimization that stopped firing usually shows
    # up in all of them.
    smallest = min(changed, key=lambda line: int(SIZED.match(line[1:]).group(1)))
    method = NAMED.match(smallest[1:]).group(1)
    print(f"Diff {method}")
    for line in difflib.unified_diff(
            disassembly(args.runtime, args.corpus, method, seq_points=False),
            disassembly(args.runtime, args.corpus, method, seq_points=True),
            fromfile="without OP_IL_SEQ_POINT", tofile="with OP_IL_SEQ_POINT",
            lineterm=""):
        print(line)
    return 1


if __name__ == "__main__":
    sys.exit(main())
