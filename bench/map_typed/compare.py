#!/usr/bin/env python3
"""Compare two Rayforce builds in alternating fresh processes."""
import argparse
import json
import os
from pathlib import Path
import re
import statistics
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--before', type=Path, required=True)
parser.add_argument('--after', type=Path, required=True)
parser.add_argument('--samples', type=int, default=5)
parser.add_argument('--n', type=int, default=100000)
parser.add_argument('--reps', type=int, default=5)
parser.add_argument('--output', type=Path)
args = parser.parse_args()
if min(args.samples, args.n, args.reps) < 1:
    parser.error('samples, n and reps must be positive')
workload = Path(__file__).with_name('map_typed.rfl')
pattern = re.compile(r'mode=(\w+) ms=([\d.eE+-]+) peak_bytes=(\d+) checksum=([\d.eE+-]+)\s*')
modes = ('typed', 'postcast', 'mixed', 'late', 'nested', 'strings')
rows = []
for sample in range(args.samples):
    for mode in modes:
        order = ('before', 'after') if sample % 2 == 0 else ('after', 'before')
        for variant in order:
            binary = getattr(args, variant).resolve()
            env = dict(os.environ, MODE=mode, N=str(args.n), REPS=str(args.reps))
            output = subprocess.check_output(
                [str(binary), '-c', '1', '-f', str(workload)], env=env,
                stdin=subprocess.DEVNULL, text=True)
            match = pattern.fullmatch(output)
            if not match or match[1] != mode:
                raise RuntimeError(f'unexpected benchmark output: {output!r}')
            rows.append(dict(sample=sample, variant=variant, mode=mode,
                             ms=float(match[2]), peak_bytes=int(match[3]),
                             checksum=float(match[4])))
if args.output:
    args.output.write_text(json.dumps(dict(n=args.n, reps=args.reps, samples=rows), indent=2) + '\n')
print('| Mode | Before ms | After ms | Before peak bytes | After peak bytes |')
print('|---|---:|---:|---:|---:|')
for mode in modes:
    cells = []
    for variant in ('before', 'after'):
        group = [row for row in rows if row['mode'] == mode and row['variant'] == variant]
        cells.append((statistics.median(row['ms'] for row in group),
                      max(row['peak_bytes'] for row in group)))
    print(f'| {mode} | {cells[0][0]:.3f} | {cells[1][0]:.3f} | {cells[0][1]} | {cells[1][1]} |')
