#!/usr/bin/env python3
"""Unix-only, stdlib benchmark. Does not build or modify either binary."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import re
import resource
import statistics
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument('baseline', type=Path)
parser.add_argument('candidate', type=Path)
parser.add_argument('--cores', type=int, nargs='+', default=[1, 8, 16])
parser.add_argument('--trials', type=int, default=3)
parser.add_argument('--output', type=Path, default=Path(__file__).with_name('results.json'))
args = parser.parse_args()
if args.trials < 1 or any(c < 1 for c in args.cores):
    parser.error('trials and total-participant core counts must be positive')
binaries = {name: path.resolve() for name, path in
            [('baseline', args.baseline), ('candidate', args.candidate)]}
probe = Path(__file__).with_name('probe.rfl').resolve()
metadata = {name: {'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
            for name, path in binaries.items()}
jobs = [(name, cores, trial) for name in binaries for cores in args.cores
        for trial in range(args.trials)]
random.Random(599).shuffle(jobs)
env = os.environ.copy()
for name in ['RAYFORCE_CORES', 'RAY_POOL_TRACE']:
    env.pop(name, None)
env['RAY_POOL_STEAL'] = '1'
rows = []
for name, cores, trial in jobs:
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    start = time.monotonic()
    proc = subprocess.run([str(binaries[name]), '-c', str(cores), str(probe)],
                          stdin=subprocess.DEVNULL, capture_output=True,
                          text=True, env=env, timeout=180)
    wall = time.monotonic() - start
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    if proc.returncode:
        raise RuntimeError(f'{name}, -c {cores}: {proc.stdout}\n{proc.stderr}')
    if 'keys: ["INST0" "INST1" "INST2"]' not in proc.stdout:
        raise RuntimeError(f'unexpected build keys: {proc.stdout}')
    count = re.search(r'^result: (\d+)$', proc.stdout, re.M)
    # 130000 rows cycling773 values: each of keys0/1/2 occurs169 times.
    expected = 200 * sum(130000 // 773 + (i < 130000 % 773) for i in range(3))
    elapsed = re.search(r'^loop_ms: ([\d.e+-]+)$', proc.stdout, re.M)
    if not count or int(count.group(1)) != expected or not elapsed:
        raise RuntimeError(f'output/oracle mismatch: {proc.stdout}')
    row = dict(binary=name, cores=cores, trial=trial, wall_s=wall,
               user_s=after.ru_utime-before.ru_utime,
               sys_s=after.ru_stime-before.ru_stime,
               loop_ms=float(elapsed.group(1)), result=expected)
    rows.append(row)
    print(json.dumps(row), flush=True)
args.output.write_text(json.dumps(dict(binaries=metadata,
    probe_sha256=hashlib.sha256(probe.read_bytes()).hexdigest(),
    environment=dict(scheduler='steal', explicit_warmup=False), rows=rows), indent=2))
for name in binaries:
    for cores in args.cores:
        group = [r for r in rows if r['binary'] == name and r['cores'] == cores]
        print(name, cores, {key: statistics.median(r[key] for r in group)
              for key in ['user_s', 'sys_s', 'wall_s', 'loop_ms']})
