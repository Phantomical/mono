"""Collect a run's per-test durations into the table the sharder reads back.

A test that went red still contributes its duration, so balance does not decay
while a failure is open.
"""

import argparse
import glob
import os
import sys
import xml.etree.ElementTree as ET


def durations(root):
    costs = {}
    for path in glob.glob(os.path.join(root, "**", "junit-*.xml"), recursive=True):
        try:
            tree = ET.parse(path)
        except ET.ParseError as exc:
            print(f"::warning::{path}: {exc}", file=sys.stderr)
            continue
        for case in tree.getroot().iter("testcase"):
            name = case.get("name")
            if name:
                costs[name] = float(case.get("time") or 0.0)
    return costs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    costs = durations(args.results)
    if not costs:
        sys.exit(f"no junit-*.xml with test cases under {args.results}")

    with open(args.out, "w", encoding="utf-8", newline="") as f:
        for name in sorted(costs):
            f.write(f"{costs[name]:.3f}\t{name}\n")
    print(f"{len(costs)} tests, {sum(costs.values()):.0f}s of test time")


if __name__ == "__main__":
    main()
