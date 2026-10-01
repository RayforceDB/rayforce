#!/usr/bin/env python3
"""Time dependency-free Parquet scans on a bounded ClickBench sample.

Build Rayforce in release mode first. Reports in-process warm medians, excluding
one warmup per case. This does not replace the full ClickBench workload.
"""
import argparse
import json
import statistics
import subprocess
import tempfile
from pathlib import Path


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('parquet',type=Path)
    ap.add_argument('--rayforce',type=Path,default=Path('./rayforce'))
    ap.add_argument('--cores',type=int,nargs='+',default=[1,4])
    ap.add_argument('--repeats',type=int,default=5)
    a = ap.parse_args()
    if a.repeats < 1 or any(c < 1 for c in a.cores): ap.error('repeats and cores must be positive')
    path = json.dumps(str(a.parquet.resolve()))
    cases = {
        'scan_all_105_columns': f'(.parquet.each {path} (fn [b] (count b)))',
        'scan_9_columns': f'(.parquet.each [CounterID AdvEngineID ResolutionWidth UserID RegionID EventDate EventTime URL Title] {path} (fn [b] (count b)))',
        'lazy_footer_count': '(select {from: source n: (count WatchID)})',
        'lazy_stream_aggregates': '(select {from: source s: (sum JavaEnable) lo: (min EventTime) hi: (max EventTime)})',
    }
    report = []
    with tempfile.TemporaryDirectory(prefix='rayforce-parquet-bench-') as tmp:
        script = Path(tmp)/'bench.rfl'
        for cores in a.cores:
            for name, expr in cases.items():
                lines = [f'(set source (.parquet.scan {path}))',expr]
                lines += [f'(println (timeit {expr}))']*a.repeats
                lines.append('(exit 0)'); script.write_text('\n'.join(lines)+'\n')
                run = subprocess.run([str(a.rayforce.resolve()),'-c',str(cores),str(script)],check=True,capture_output=True,text=True)
                times = [float(x) for x in run.stdout.splitlines() if x.strip()]
                if len(times) != a.repeats: raise RuntimeError(f'unexpected benchmark output: {run.stdout}')
                report.append({'case':name,'cores':cores,'median_ms':statistics.median(times),'runs_ms':times})
    print(json.dumps({'source':str(a.parquet.resolve()),'warmup_runs':1,'results':report},indent=2))

if __name__ == '__main__':
    main()
