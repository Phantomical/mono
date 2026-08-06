#!/usr/bin/env python3
"""Pull per-test wall times out of a CTest LastTest.log, slowest first.

    python3 extract-times.py <LastTest.log> [N]

CTest's own stdout summary is not kept anywhere, so this log is the only
record of what each test cost.  Note that `ctest -N` overwrites LastTest.log
even though it runs nothing -- copy the log out of the build tree before
doing anything else with that tree.
"""
import re, sys

path = sys.argv[1]
top = int(sys.argv[2]) if len(sys.argv) > 2 else 40

name, rows = None, []
for line in open(path, errors='replace'):
    m = re.match(r'\s*\d+/\d+ Testing: (.+?)\s*$', line)
    if m:
        name = m.group(1)
        continue
    m = re.match(r'\s*Test time =\s+([\d.]+) sec', line)
    if m and name:
        rows.append((float(m.group(1)), name))
        name = None

rows.sort(reverse=True)
print(f"{len(rows)} tests")
for t, n in rows[:top]:
    print(f"{t:9.2f}  {n}")
