#!/usr/bin/env python3
"""Assert the perf jit dump names every body of the fixture, under one name per
method whichever engine emitted it, and gives each range of JIT'd code to
exactly one record.

perf resolves a sample against the range of the record holding the address, and
the error a wrong range makes is invisible from the report: the names are real
methods and the totals are plausible.  A tier-1 promotion links up to a batch's
worth of methods into one object, and the dump writer publishes that whole
object as the runs its layout actually has - so a batch neighbour with nothing
between it and the next is described by the same record, under one of the
batch's names, rather than by a record of its own.

The rule the ranges owe is a partition: the ranges do not overlap, and none of
them sits inside another.  A record's code_size then covers only code this run's
compile actually placed there, and no sample's name rests on the order two
records were written in.

The rule the names owe is that a method has one.  A body the classic compiler
emitted at tier 0 and the body the backend compiles for the same method print
under the same name, so a profile adds the two up rather than listing a method
twice - or listing its tier-0 half as a bare address.  The corpus runs three
times to check that: once at the default tiers, once with promotion off, where
every fixture body is a classic one, and once with tier 0 off, where every one
is the backend's.  The two single-engine runs have to name the same set, and
each classic body has to carry a frame description, or a stack walk stops at it.

What stands in for "the fixture's bodies reached tier 1" in the default run is
the function count a record's own unwinding info carries (one FDE per function
described), summed over the fixture records describing more than one: a
classic body is one function under one record, so only a promoted batch
describes several under one name.
"""

import argparse
import os
import struct
import subprocess
import sys

JIT_CODE_LOAD = 0
JIT_CODE_UNWINDING_INFO = 4

# id, total size and timestamp, then pid, tid, vma, code address, code size and
# code index (JitCodeLoadRecord, mono/mini/mini-runtime.c).
RECORD_HEADER = 16
LOAD_FIELDS = struct.Struct("<IIQQQQ")

# unwinding_data_size, eh_frame_hdr_size and mapped_size
# (mono::perf::unwinding_record, mono/llvm/debugging/perf/jitdump.cpp).
UNWIND_FIELDS = struct.Struct("<QQQ")

# eh_frame_hdr's own header - version, three encoding bytes, then the function
# count (mono::perf::assemble, mono/llvm/debugging/perf/eh-frame.cpp). A
# record with no describable function writes no unwinding-info record at all,
# so this is never read for one that carries one.
EH_FRAME_HDR_COUNT_OFFSET = 8

# The fixture's own bodies: Work<T>'s four methods over its sixteen struct
# instantiations, each a body of its own.
FIXTURE_PREFIX = "Work`1<"
WANT_BODIES = 64

# How many of the fixture's own functions have to be described by batch records
# in the default run before it says anything.  The batch size defaults to 32,
# so this is one full batch.
WANT_BATCHED = 32

# Promotion off: every fixture body stays a classic tier-0 one.
TIER0_ONLY = "--llvm-opt=-mono-tier1-threshold=0"
# Tier 0 off: every fixture body is compiled by the backend, on its own.
BACKEND_ONLY = "--llvm-opt=-mono-tier0-filter=0"


def die(message, *details):
    """Report a problem with the run itself, rather than with what it reported."""
    print(f"{os.path.basename(sys.argv[0])}: {message}", file=sys.stderr)
    for detail in details:
        print(f"  {detail}", file=sys.stderr)
    sys.exit(1)


def records(path):
    """Read every JIT_CODE_LOAD record as (address, size, name, functions),
    functions being the count its immediately preceding JIT_CODE_UNWINDING_INFO
    record describes, or 0 where the record has none (mono::perf::write ()
    pairs the two, or writes the load alone where the object has nothing
    describable at that address)."""
    with open(path, "rb") as handle:
        data = handle.read()
    if len(data) < 40:
        die(f"{path} is too short to hold a jit dump header")

    header_size = struct.unpack_from("<I", data, 8)[0]
    at = header_size
    pending_functions = 0
    while at + RECORD_HEADER <= len(data):
        kind, total = struct.unpack_from("<II", data, at)
        if total < RECORD_HEADER or at + total > len(data):
            die(f"{path} ends inside a record at offset {at}")
        body = data[at + RECORD_HEADER : at + total]
        if kind == JIT_CODE_UNWINDING_INFO:
            unwind_size, ehdr_size, _mapped_size = UNWIND_FIELDS.unpack_from(body, 0)
            hdr = body[UNWIND_FIELDS.size : UNWIND_FIELDS.size + unwind_size][-ehdr_size:]
            pending_functions = struct.unpack_from(
                "<I", hdr, EH_FRAME_HDR_COUNT_OFFSET)[0]
        elif kind == JIT_CODE_LOAD:
            _, _, _, address, size, _ = LOAD_FIELDS.unpack_from(body, 0)
            name = body[LOAD_FIELDS.size : body.index(b"\0", LOAD_FIELDS.size)]
            yield address, size, name.decode("utf-8", "replace"), pending_functions
            pending_functions = 0
        at += total


def run(runtime, corpus, *options):
    """Run the corpus under --jitdump and return its records sorted by address."""
    # The wrapper execs the runtime, so the dump the runtime opens is named for
    # the pid this call gets back.
    proc = subprocess.Popen([runtime, "--jitdump", *options, corpus],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            universal_newlines=True)
    out, err = proc.communicate()
    dump = f"/tmp/jit-{proc.pid}.dump"
    if proc.returncode != 0:
        die(f"the corpus run {' '.join(options)} failed (exit {proc.returncode})",
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
    return loaded


def partitioned(loaded):
    """Return one message for each break of the partition rule."""
    issues = []
    holder = None
    for address, size, name, _functions in loaded:
        if holder is not None:
            at, end, whose = holder
            if address < end:
                shape = "sits inside" if address + size <= end else "overlaps"
                issues.append(f"{name} at 0x{address:x} + {size} {shape} "
                              f"{whose} at 0x{at:x} + {end - at}")
        if holder is None or address + size > holder[1]:
            holder = (address, address + size, name)
    return issues


def fixture_bodies(loaded):
    """The (name, functions) of every record led by one of the fixture's bodies."""
    return [(name, functions) for _, _, name, functions in loaded
            if name.startswith(FIXTURE_PREFIX)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runtime", help="the mono binary, or the mono-wrapper script")
    parser.add_argument("corpus", help="the corpus .exe to run")
    args = parser.parse_args()

    runs = {
        "default": run(args.runtime, args.corpus),
        "tier 0 only": run(args.runtime, args.corpus, TIER0_ONLY),
        "backend only": run(args.runtime, args.corpus, BACKEND_ONLY),
    }

    batched = sum(functions for _, functions in fixture_bodies(runs["default"])
                  if functions >= 2)
    if batched < WANT_BATCHED:
        die(f"only {batched} of the fixture's own functions sit in batch records",
            f"The partition rule is about a compile batch, which is {WANT_BATCHED}",
            "methods, so a run this small does not measure it.")

    issues = []
    for label, loaded in runs.items():
        issues += [f"{label}: {issue}" for issue in partitioned(loaded)]

    tier0 = fixture_bodies(runs["tier 0 only"])
    backend = fixture_bodies(runs["backend only"])
    tier0_names = {name for name, _ in tier0}
    backend_names = {name for name, _ in backend}
    if len(tier0_names) < WANT_BODIES:
        issues.append(f"tier 0 only: {len(tier0_names)} of the fixture's "
                      f"{WANT_BODIES} bodies are named")
    if len(backend_names) < WANT_BODIES:
        issues.append(f"backend only: {len(backend_names)} of the fixture's "
                      f"{WANT_BODIES} bodies are named")
    for name in sorted(tier0_names - backend_names):
        issues.append(f"{name} is named at tier 0 and not by the backend")
    for name in sorted(backend_names - tier0_names):
        issues.append(f"{name} is named by the backend and not at tier 0")
    for name, functions in tier0:
        if functions == 0:
            issues.append(f"{name} at tier 0 carries no frame description")

    for issue in issues:
        print(f"  FAIL {issue}")
    print(f"{sum(len(loaded) for loaded in runs.values())} records over "
          f"{len(runs)} runs, {batched} of the fixture's own functions in batch "
          f"records, {len(tier0_names)} bodies named at tier 0 and "
          f"{len(backend_names)} by the backend, {len(issues)} failed")
    return 1 if issues else 0


if __name__ == "__main__":
    sys.exit(main())
