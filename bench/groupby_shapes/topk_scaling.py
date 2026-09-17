#!/usr/bin/env python3
"""Compare indexed top/bottom-K kernels against an independent histogram oracle.

Build each checkout with `make lib`, then compile topk_consumer.c against that
checkout's librayforce.a, using -Iinclude -Isrc -lm -lpthread. This Linux harness
reports ru_maxrss in KiB. Query-language K limits are not changed by this test.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--rows', type=int, default=4000003)
    parser.add_argument('--ks', default='1024,65536,4000003')
    parser.add_argument('--baseline-skip-ks', default='', help='K values with a recorded baseline correctness failure')
    parser.add_argument('--workers', default='1,2,4,8,16,default')
    parser.add_argument('--rounds', type=int, default=3)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    ks, workers = [int(k) for k in args.ks.split(',')], args.workers.split(',')
    if args.rows < 1 or args.rounds < 1 or any(k < 1 for k in ks):
        parser.error('rows, rounds and K must be positive')
    if any(w != 'default' and (not w.isdecimal() or int(w) < 1) for w in workers):
        parser.error('workers must be positive or default')
    baseline_skip = {int(k) for k in args.baseline_skip_ks.split(',') if k}
    if baseline_skip - set(ks):
        parser.error('baseline skip K must be in --ks')
    binaries = [('baseline', args.baseline.resolve()), ('current', args.binary.resolve())]
    hashes = {name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in binaries}
    matrix = [(k, worker, name, path) for k in ks for worker in workers for name, path in binaries
              if not (name == 'baseline' and k in baseline_skip)]
    records = []
    for round_number in range(1, args.rounds + 1):
        for k, worker, name, path in matrix if round_number % 2 else reversed(matrix):
            env = os.environ.copy()
            env.pop('RAYFORCE_CORES', None)
            if worker != 'default':
                env['RAYFORCE_CORES'] = worker
            run = subprocess.run([str(path), str(args.rows), str(k)], env=env,
                                 capture_output=True, text=True, check=True)
            result = json.loads(run.stdout)
            if worker != 'default' and result['actual_workers'] != int(worker):
                raise RuntimeError('worker setting was not honored')
            result.update(case=f'topk-k{k}', workers=worker, binary=name,
                          round=round_number, rows=args.rows, k=k, sha256=hashes[name])
            records.append(result)
            args.output.write_text(json.dumps(records, indent=2) + '\n')
            print(f'{name} K={k} workers={worker} round={round_number}: '
                  f'warm={statistics.median(result["warm_ms"]):.3f} ms', flush=True)
    print(f'Histogram oracle matched all {len(records)} runs.')


if __name__ == '__main__':
    main()
