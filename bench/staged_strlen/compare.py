#!/usr/bin/env python3
"""Optional standard-library runner; execute after building two release binaries."""
import argparse
import json
from pathlib import Path
import random
import re
import statistics
import subprocess

parser = argparse.ArgumentParser(__doc__)
parser.add_argument("base", type=Path)
parser.add_argument("candidate", type=Path)
parser.add_argument("--cores", type=int, default=8)
parser.add_argument("--trials", type=int, default=3)
parser.add_argument("--setup", action="store_true")
args = parser.parse_args()
if args.cores < 1 or args.trials < 1:
    parser.error("cores and trials must be positive")
root = Path(__file__).resolve().parents[2]
executables = {"base": args.base.resolve(), "candidate": args.candidate.resolve()}

def run(executable, script):
    return subprocess.run([str(executable), "-c", str(args.cores), str(script)],
                          cwd=root, stdin=subprocess.DEVNULL, text=True,
                          capture_output=True, check=True).stdout

if args.setup:
    print(run(executables["base"], root / "bench/staged_strlen/setup.rfl"), flush=True)
tasks = [(i, name, gen) for i in range(args.trials)
         for name in executables for gen in (0, 2)]
random.Random(902512).shuffle(tasks)
results = []
for trial, name, gen in tasks:
    stdout = run(executables[name], root / f"bench/staged_strlen/gen{gen}.rfl")
    checksum = re.search(r"^checksum: (.+)$", stdout, re.M)
    times = re.search(r"^ms: \[(.+)\]$", stdout, re.M)
    if not checksum or not times:
        raise RuntimeError(f"unexpected output: {stdout!r}")
    values = [float(v) for v in times.group(1).split()]
    if len(values) != 7:
        raise RuntimeError(f"expected seven timings, got {values!r}")
    results.append(dict(trial=trial, binary=name, gen=gen, ms=values,
                        median_ms=statistics.median(values), checksum=checksum.group(1)))
# A rendered checksum is only a quick sanity check, not answer/bitwise equivalence.
if len({r["checksum"] for r in results}) != 1:
    raise RuntimeError("checksums differ; investigate answers before comparing timings")
print(json.dumps(results, indent=2), flush=True)
for name in executables:
    for gen in (0, 2):
        medians = [r["median_ms"] for r in results if r["binary"] == name and r["gen"] == gen]
        print(f"{name} gen{gen}: trial medians={medians}, median={statistics.median(medians):.3f} ms")
