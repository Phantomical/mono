#!/usr/bin/env python3
"""Assert the perf jit dump names every body of the fixture under one name per
method whichever engine emitted it, describes it with one record per function,
and gives each range of JIT'd code to exactly one record.

perf resolves a sample against the range of the record holding the address, and
the error a wrong range makes is invisible from the report: the names are real
methods and the totals are plausible.  A tier-1 promotion links up to a batch's
worth of methods into one object, and the dump writer publishes each function of
that object as a record of its own, so a sample in a batch neighbour prints
under the neighbour's name and not under whichever member the object put first.

The rule the ranges owe is a partition: the ranges do not overlap, and none of
them sits inside another.  A record's code_size then covers only code this run's
compile actually placed there, and no sample's name rests on the order two
records were written in.  The rule the records owe is one function each: a
record describing two is naming one of them after the other.

The rule the names owe is that a method has one.  A body the classic compiler
emitted at tier 0 and the body the backend compiles for the same method print
under the same name, so a profile adds the two up rather than listing a method
twice - or listing its tier-0 half as a bare address.  The corpus runs three
times to check that: once at the default tiers, once with promotion off, where
every fixture body is a classic one, and once with tier 0 off, where every one
is the backend's.  The two single-engine runs have to name the same set, and
every fixture record in every run has to carry a frame description, or a stack
walk stops at it: a backend body whose record has no room past it for one is
published without it.

What stands in for "the fixture's bodies reached tier 1" in the default run is
a fixture name published twice: once for the classic body and once for the
backend's, at two addresses.
"""

import argparse
import collections
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

# How many of the fixture's bodies have to reach the backend in the default run
# before it says anything.  The batch size defaults to 32, so this is one full
# batch.
WANT_PROMOTED = 32

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


def one_function_each(loaded):
    """Return one message for each record describing more than one function."""
    return [f"{name} at 0x{address:x} describes {functions} functions"
            for address, _, name, functions in loaded if functions >= 2]


def fixture_bodies(loaded):
    """The (name, functions) of every record for one of the fixture's bodies."""
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

    published = collections.Counter(name for name, _ in fixture_bodies(runs["default"]))
    promoted = sum(1 for count in published.values() if count >= 2)
    if promoted < WANT_PROMOTED:
        die(f"only {promoted} of the fixture's bodies were published twice in the "
            "default run",
            f"A compile batch is {WANT_PROMOTED} methods, so a run that promotes",
            "fewer does not reach the object the record-per-function rule is about.")

    issues = []
    for label, loaded in runs.items():
        issues += [f"{label}: {issue}" for issue in partitioned(loaded)]
        issues += [f"{label}: {issue}" for issue in one_function_each(loaded)]
        for name, functions in fixture_bodies(loaded):
            if functions == 0:
                issues.append(f"{label}: {name} carries no frame description")

    tier0_names = {name for name, _ in fixture_bodies(runs["tier 0 only"])}
    backend_names = {name for name, _ in fixture_bodies(runs["backend only"])}
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

    for issue in issues:
        print(f"  FAIL {issue}")
    print(f"{sum(len(loaded) for loaded in runs.values())} records over "
          f"{len(runs)} runs, {promoted} of the fixture's bodies published by "
          f"both engines, {len(tier0_names)} bodies named at tier 0 and "
          f"{len(backend_names)} by the backend, {len(issues)} failed")
    return 1 if issues else 0


if __name__ == "__main__":
    sys.exit(main())
