#!/usr/bin/env python3
"""Summarize fresh-process grouping measurements without discarding outliers."""
import argparse
import csv
import json
from pathlib import Path
import statistics


def summarize(records):
    groups = {}
    for row in records:
        groups.setdefault((row['case'], row['workers'], row['binary']), []).append(row)
    for (case, workers, binary), rows in groups.items():
        warm = [statistics.median(row['warm_ms']) for row in rows]
        cold = [row['cold_ms'] for row in rows]
        yield dict(case=case, workers=workers, binary=binary, processes=len(rows),
                   cold_median_ms=statistics.median(cold), cold_min_ms=min(cold), cold_max_ms=max(cold),
                   warm_median_ms=statistics.median(warm), warm_min_ms=min(warm), warm_max_ms=max(warm),
                   peak_rss_kib=max(row['peak_rss_kib'] for row in rows))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--process-output', type=Path,
                        help='also retain each process and all five warm timings')
    args = parser.parse_args()
    records = json.loads(args.input.read_text())
    rows = list(summarize(records))
    if not rows:
        parser.error('no measurements')
    with args.output.open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    if args.process_output:
        process_rows = []
        for record in records:
            row = {key: record[key] for key in (
                'case', 'workers', 'binary', 'sha256', 'round', 'rows', 'groups',
                'cold_ms', 'peak_rss_kib', 'result_sha256')}
            row['warm_median_ms'] = statistics.median(record['warm_ms'])
            row.update({f'warm_{i + 1}_ms': value for i, value in enumerate(record['warm_ms'])})
            process_rows.append(row)
        with args.process_output.open('w', newline='') as stream:
            writer = csv.DictWriter(stream, fieldnames=list(process_rows[0]))
            writer.writeheader()
            writer.writerows(process_rows)
    pairs = {(row['case'], row['workers'], row['binary']): row for row in rows}
    for row in rows:
        old = pairs.get((row['case'], row['workers'], 'baseline'))
        if row['binary'] != 'current' or not old:
            continue
        ratio = old['warm_median_ms'] / row['warm_median_ms']
        print(f"{row['case']:24} {row['workers']:>7} "
              f"{old['warm_median_ms']:9.3f} -> {row['warm_median_ms']:9.3f} ms "
              f"({ratio:.2f}x; current process medians {row['warm_min_ms']:.3f}..{row['warm_max_ms']:.3f})")


if __name__ == '__main__':
    main()
