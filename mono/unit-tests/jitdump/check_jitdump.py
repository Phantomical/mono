#!/usr/bin/env python3
"""Assert the perf jit dump gives each range of JIT'd code to exactly one name.

perf resolves a sample against the range of the record holding the address, and
the error a wrong range makes is invisible from the report: the names are real
methods and the totals are plausible.  A tier-1 promotion links up to
MONO_LLVM_JIT_BATCH methods into one object, which is where the neighbours are
other methods.

The rule is a partition: the ranges do not overlap, and none of them sits inside
another.  A record's code_size is then the method's own code size, and no
sample's name rests on the order two records were written in.

The fixture has to reach tier 1 for any of this to be measured, so the run also
has to show that it did.
"""

import argparse
import os
import struct
import subprocess
import sys

JIT_CODE_LOAD = 0

# id, total size and timestamp, then pid, tid, vma, code address, code size and
# code index (JitCodeLoadRecord, mono/mini/mini-runtime.c).
RECORD_HEADER = 16
LOAD_FIELDS = struct.Struct("<IIQQQQ")

# How many of the fixture's own bodies have to reach tier 1 before the run says
# anything.  MONO_LLVM_JIT_BATCH defaults to 32, so this is one full batch.
WANT_COMPILED = 32


def die(message, *details):
    """Report a problem with the run itself, rather than with what it reported."""
    print(f"{os.path.basename(sys.argv[0])}: {message}", file=sys.stderr)
    for detail in details:
        print(f"  {detail}", file=sys.stderr)
    sys.exit(1)


def records(path):
    """Read every JIT_CODE_LOAD record as (address, size, name)."""
    with open(path, "rb") as handle:
        data = handle.read()
    if len(data) < 40:
        die(f"{path} is too short to hold a jit dump header")

    header_size = struct.unpack_from("<I", data, 8)[0]
    at = header_size
    while at + RECORD_HEADER <= len(data):
        kind, total = struct.unpack_from("<II", data, at)
        if total < RECORD_HEADER or at + total > len(data):
            die(f"{path} ends inside a record at offset {at}")
        if kind == JIT_CODE_LOAD:
            body = data[at + RECORD_HEADER : at + total]
            _, _, _, address, size, _ = LOAD_FIELDS.unpack_from(body, 0)
            name = body[LOAD_FIELDS.size : body.index(b"\0", LOAD_FIELDS.size)]
            yield address, size, name.decode("utf-8", "replace")
        at += total


def partitioned(loaded):
    """Return one message for each break of the partition rule."""
    issues = []
    holder = None
    for address, size, name in loaded:
        if holder is not None:
            at, end, whose = holder
            if address < end:
                shape = "sits inside" if address + size <= end else "overlaps"
                issues.append(f"{name} at 0x{address:x} + {size} {shape} "
                              f"{whose} at 0x{at:x} + {end - at}")
        if holder is None or address + size > holder[1]:
            holder = (address, address + size, name)
    return issues


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runtime", help="the mono binary, or the mono-wrapper script")
    parser.add_argument("corpus", help="the corpus .exe to run")
    args = parser.parse_args()

    # The wrapper execs the runtime, so the dump the runtime opens is named for
    # the pid this call gets back.
    proc = subprocess.Popen([args.runtime, "--jitdump", args.corpus],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            universal_newlines=True)
    out, err = proc.communicate()
    dump = f"/tmp/jit-{proc.pid}.dump"
    if proc.returncode != 0:
        die(f"the corpus run failed (exit {proc.returncode})",
            *err.splitlines()[-20:])
    if not os.path.isfile(dump):
        die(f"the run wrote no {dump}",
            "--jitdump is what opens it, and the runtime has to be built with it.")

    try:
        loaded = sorted(records(dump), key=lambda r: (r[0], r[1]))
    finally:
        os.unlink(dump)

    if not loaded:
        die(f"{dump} holds no JIT_CODE_LOAD record")

    compiled = sum(1 for _, _, name in loaded if name.startswith("Work`1<"))
    if compiled < WANT_COMPILED:
        die(f"only {compiled} of the fixture's own bodies reached tier 1",
            f"The rule below is about a compile batch, which is {WANT_COMPILED}",
            "methods, so a run this small does not measure it.")

    issues = partitioned(loaded)
    for issue in issues:
        print(f"  FAIL {issue}")
    print(f"{len(loaded)} records, {compiled} of them the fixture's own bodies, "
          f"{len(issues)} failed")
    return 1 if issues else 0


if __name__ == "__main__":
    sys.exit(main())
