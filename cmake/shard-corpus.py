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
shared support directory, and each shard reaches them at the paths the cases
reference.  Staging those is a second pass, --stage-support, which the build
runs once it has built them: a shard holds hard links on a host that gives an
unprivileged process no symlink, and a hard link cannot be made before its
target exists.

Usage:
  shard-corpus.py --src <corpus dir> --dest <binary dir> --mode pos|neg
                  --shards <n> [--stage-support]

Stages <dest>/run-00 .. run-<n-1> and <dest>/support/dlls, and writes the
assignment to <dest>/shards.txt for the curious.
"""

import argparse
import os
import re
import shutil
import sys
from pathlib import Path


def stage_file(target, link):
    """Puts target at link, by whichever of the three a host gives us.

    A symlink where the host makes one. Windows gives an unprivileged process
    none, so a shard there holds hard links, and a copy stands in when target
    is on another volume. Both need target to exist, which is why the support
    assemblies are staged in a pass of their own.
    """
    if link.is_symlink() or link.is_file():
        link.unlink()

    try:
        link.symlink_to(target)
        return
    except OSError:
        pass

    try:
        os.link(target, link)
    except OSError:
        shutil.copy2(target, link)


def stage_dir(target, link):
    """Puts directory target at link, by symlink or by a Windows junction.

    Neither one copies, so what the build writes into target later is visible
    through link.
    """
    if link.is_symlink():
        link.unlink()
    elif link.is_dir():
        # A junction reads as an ordinary directory, so this is either one we
        # made or a real directory a caller wants kept. Leave it either way.
        return

    try:
        link.symlink_to(target, target_is_directory=True)
    except OSError:
        import _winapi
        _winapi.CreateJunction(str(target), str(link))


def clear_staged(rundir):
    """Takes out what an earlier run staged, so a changed assignment does not
    leave a case behind in the shard it moved out of.

    A junction is removed rather than walked. Deleting through one would take
    the support tree with it.
    """
    for old in rundir.iterdir():
        if old.is_symlink():
            old.unlink()
        elif old.is_dir():
            if os.path.isjunction(old):
                old.rmdir()
        else:
            old.unlink()


def support_names(src, mode):
    """The support assemblies a shard reaches by name: in the errors corpus the
    build compiles every *-lib.cs / *-module.cs (compiler-tester skips them:
    uppercase); in both corpora it assembles every root *-lib.il.
    """
    names = [p.stem + ".dll" for p in src.glob("*-lib.il")]
    if mode == "neg":
        names += [p.stem + ".dll"
                  for pat in ("*-lib.cs", "*-module.cs")
                  for p in src.glob(pat)]
    return names

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
    ap.add_argument("--stage-support", action="store_true",
                    help="stage the built support assemblies into the shards "
                         "and do nothing else; run once they are built")
    args = ap.parse_args()

    src, dest = args.src.resolve(), args.dest
    support = dest / "support"

    if args.stage_support:
        for i in range(args.shards):
            rundir = dest / f"run-{i:02d}"
            for name in support_names(src, args.mode):
                if (support / name).is_file():
                    stage_file(support / name, rundir / name)
        return

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

    # The shared support tree carries the source side of dlls/ too, so a shard
    # sees one merged dlls/ directory of sources and built assemblies.
    if (src / "dlls").is_dir():
        for p in (src / "dlls").rglob("*"):
            if p.is_file():
                link = support / "dlls" / p.relative_to(src / "dlls")
                link.parent.mkdir(parents=True, exist_ok=True)
                stage_file(p, link)

    for stale in dest.glob("run-*"):
        if stale.is_dir() and int(stale.name[4:]) >= args.shards:
            shutil.rmtree(stale)

    for i, members in enumerate(bins):
        rundir = dest / f"run-{i:02d}"
        rundir.mkdir(parents=True, exist_ok=True)
        clear_staged(rundir)
        for f in members:
            stage_file(src / f, rundir / f)
        for p in aux:
            stage_file(p, rundir / p.name)
        if (src / "dlls").is_dir():
            stage_dir(support / "dlls", rundir / "dlls")

    # The support assemblies are not staged here. They are built after this
    # runs, and a hard link needs the file to exist, so --stage-support puts
    # them in once the build has made them.

    with open(dest / "shards.txt", "w") as f:
        for i, members in enumerate(bins):
            for m in members:
                f.write(f"{i:02d} {m}\n")

    counts = sorted(len(b) for b in bins)
    print(f"sharded {len(cases)} cases into {args.shards} directories "
          f"({counts[0]}..{counts[-1]} per shard)")


if __name__ == "__main__":
    main()
