#!/usr/bin/env python3
"""Stage the compiler test corpus into shard directories.

compiler-tester globs its cases out of the working directory and runs them
sequentially in one process, so the unit of parallelism is the directory: this
script deals the corpus into N staging directories (run-00 .. run-NN), each a
directory of symlinks, and CTest runs one compiler-tester per directory.

A case is not always self-contained: its `// Compiler options:` header can
name a library another case builds (-r:foo-lib.dll), a module (-addmodule:),
or a second source file compiled into the same program.  Cases connected that
way must land in the same shard, in any order -- compiler-tester itself sorts
-lib/-mod cases first within a run.  Everything else in the corpus (expected
outputs, .inc includes, keys, sources that are not cases themselves) is inert
and is staged into every shard.

Support assemblies prebuilt by the build (out of *-lib.il, and in the errors
suite every uppercase CS*-lib.cs, which compiler-tester skips) live in a
shared support directory; each shard gets symlinks to them at the paths the
cases reference.  The links dangle until the build has run, which is fine:
nothing reads them at configure time.

Usage:
  shard-corpus.py --src <corpus dir> --dest <binary dir> --mode pos|neg
                  --shards <n>

Stages <dest>/run-00 .. run-<n-1> and <dest>/support/dlls, and writes the
assignment to <dest>/shards.txt for the curious.
"""

import argparse
import re
import shutil
import sys
from pathlib import Path

# The same selection compiler-tester makes for -files:v4 (its most inclusive
# version filter), including the skip of names starting with an uppercase
# letter.
PATTERNS = {
    "pos": ("test*.cs", "gtest*.cs", "dtest*.cs"),
    "neg": ("cs*.cs", "gcs*.cs", "dcs*.cs"),
}

def read_source(path):
    """The whole source of a case, '' if unreadable."""
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


# Any path-like token ending in an extension a case can pull in.  The
# character class stops at ':', '=' and ',', which is what peels the file out
# of `-r:alias=foo-lib.dll` and `-res:foo.cs,name` alike.
FILE_RE = re.compile(r"[\w./-]+\.(?:cs|il|dll|netmodule)\b")


def dependencies(source):
    """Corpus file names a case's source pulls in, wherever they appear: the
    `// Compiler options:` header names libraries, modules, resources and
    extra sources, and a body can open a sibling's output by name at run time
    (test-695.cs reads test-695-2-lib.dll through Cecil).  Scanning the whole
    text over-approximates -- a comment mentioning another case counts -- but
    a false edge only merges two shards' worth of work, never breaks one."""
    deps = []
    for ref in FILE_RE.findall(source):
        while ref.startswith("./"):
            ref = ref[2:]
        # A path with a directory is under dlls/, or an external reference;
        # both are present in every shard.
        if "/" in ref:
            continue
        if ref.endswith((".dll", ".netmodule")):
            stem = ref.rsplit(".", 1)[0]
            deps.append(stem + ".cs")
        else:
            deps.append(ref)
    return deps


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True, type=Path)
    ap.add_argument("--dest", required=True, type=Path)
    ap.add_argument("--mode", required=True, choices=("pos", "neg"))
    ap.add_argument("--shards", required=True, type=int)
    args = ap.parse_args()

    src, dest = args.src.resolve(), args.dest
    support = dest / "support"

    cases = set()
    for pat in PATTERNS[args.mode]:
        cases.update(p.name for p in src.glob(pat) if not p.name[0].isupper())
    if len(cases) < args.shards:
        sys.exit(f"{len(cases)} cases cannot fill {args.shards} shards")

    aux = [p for p in src.iterdir()
           if p.is_file() and p.name not in cases]

    # Union-find over the reference edges.  Only edges between two cases
    # constrain the split; a reference to anything else is satisfied in every
    # shard.
    parent = {c: c for c in cases}

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    for case in sorted(cases):
        for dep in dependencies(read_source(src / case)):
            if dep in cases and dep != case:
                parent[find(case)] = find(dep)

    clusters = {}
    for c in sorted(cases):
        clusters.setdefault(find(c), []).append(c)

    # Largest cluster first into the lightest shard.
    bins = [[] for _ in range(args.shards)]
    loads = [0] * args.shards
    for members in sorted(clusters.values(), key=lambda m: (-len(m), m)):
        i = loads.index(min(loads))
        bins[i].extend(sorted(members))
        loads[i] += len(members)

    # Support assemblies the shards link to by name: in the errors corpus the
    # build compiles every *-lib.cs / *-module.cs (compiler-tester skips them:
    # uppercase); in both corpora it assembles every root *-lib.il.
    prebuilt = [p.stem + ".dll" for p in src.glob("*-lib.il")]
    if args.mode == "neg":
        prebuilt += [p.stem + ".dll"
                     for pat in ("*-lib.cs", "*-module.cs")
                     for p in src.glob(pat)]

    # The shared support tree carries the source side of dlls/ too, so a shard
    # sees one merged dlls/ directory of sources and built assemblies.
    if (src / "dlls").is_dir():
        for p in (src / "dlls").rglob("*"):
            if p.is_file():
                link = support / "dlls" / p.relative_to(src / "dlls")
                link.parent.mkdir(parents=True, exist_ok=True)
                if link.is_symlink():
                    link.unlink()
                link.symlink_to(p)

    for stale in dest.glob("run-*"):
        if stale.is_dir() and int(stale.name[4:]) >= args.shards:
            shutil.rmtree(stale)

    for i, members in enumerate(bins):
        rundir = dest / f"run-{i:02d}"
        rundir.mkdir(parents=True, exist_ok=True)
        for old in rundir.iterdir():
            if old.is_symlink():
                old.unlink()
        for f in members:
            (rundir / f).symlink_to(src / f)
        for p in aux:
            (rundir / p.name).symlink_to(p)
        for name in prebuilt:
            (rundir / name).symlink_to(support / name)
        if (src / "dlls").is_dir():
            (rundir / "dlls").symlink_to(support / "dlls")

    with open(dest / "shards.txt", "w") as f:
        for i, members in enumerate(bins):
            for m in members:
                f.write(f"{i:02d} {m}\n")

    counts = sorted(len(b) for b in bins)
    print(f"sharded {len(cases)} cases into {args.shards} directories "
          f"({counts[0]}..{counts[-1]} per shard)")


if __name__ == "__main__":
    main()
