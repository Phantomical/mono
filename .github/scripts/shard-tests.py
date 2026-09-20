"""Split the test list into shards of equal measured duration.

Longest-processing-time first rather than an exact packing: exact is NP-hard
and LPT is within 4/3 of it.

Writes <out>/<n>.txt, the names ctest reads back with --tests-from-file, and
<out>/count for the shard job to check its own matrix against.
"""

import argparse
import json
import os
import statistics
import subprocess
import sys


def test_names(build, exclude_labels):
    """Every test ctest would run."""
    argv = ["ctest", "--test-dir", build, "--show-only=json-v1"]
    if exclude_labels:
        argv += ["-LE", exclude_labels]
    out = subprocess.run(argv, check=True, capture_output=True, text=True).stdout
    return [t["name"] for t in json.loads(out)["tests"]]


def read_costs(path):
    """{name: seconds} from a '<seconds>\\t<name>' table, or {} if there is none."""
    costs = {}
    if not path:
        return costs
    try:
        handle = open(path, encoding="utf-8")
    except FileNotFoundError:
        return costs
    with handle:
        for line in handle:
            secs, _, name = line.rstrip("\r\n").partition("\t")
            if name:
                costs[name] = float(secs)
    return costs


def partition(names, costs, shards):
    """Longest first onto whichever shard is least loaded."""
    # A test with no recorded duration takes the median rather than zero.
    # At zero every unknown test would land on the same shard, because adding
    # one leaves that shard still the least loaded.
    fallback = statistics.median(costs.values()) if costs else 1.0

    load = [0.0] * shards
    bins = [[] for _ in range(shards)]
    for name in sorted(names, key=lambda n: -costs.get(n, fallback)):
        i = load.index(min(load))
        bins[i].append(name)
        load[i] += costs.get(name, fallback)
    return bins, load


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    ap.add_argument("--shards", type=int, required=True)
    ap.add_argument("--costs")
    ap.add_argument("--exclude-labels", default="")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    names = test_names(args.build, args.exclude_labels)
    if not names:
        sys.exit("ctest listed no tests")
    costs = read_costs(args.costs)
    bins, load = partition(names, costs, args.shards)

    # A partition that dropped a test would go green with the test not run.
    packed = [n for b in bins for n in b]
    if sorted(packed) != sorted(names):
        sys.exit(f"partition covers {len(packed)} of {len(names)} tests")

    # newline="" so a Windows runner writes the same bytes a Linux one does.
    # The shard job compares the count against its own matrix in bash, where a
    # stray carriage return is not the number it reads.
    os.makedirs(args.out, exist_ok=True)
    for i, b in enumerate(bins, start=1):
        with open(os.path.join(args.out, f"{i}.txt"), "w",
                  encoding="utf-8", newline="") as f:
            f.write("".join(f"{n}\n" for n in b))
    with open(os.path.join(args.out, "count"), "w",
              encoding="utf-8", newline="") as f:
        f.write(f"{args.shards}\n")

    known = sum(1 for n in names if n in costs)
    print(f"{len(names)} tests over {args.shards} shards, "
          f"{known} with a recorded duration")
    if costs:
        print(f"predicted {min(load):.0f}s to {max(load):.0f}s a shard")
    else:
        print("no durations to split on, so the shards are even by count")


if __name__ == "__main__":
    main()
