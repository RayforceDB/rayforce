#!/usr/bin/env python3
"""Check lazy Parquet queries on a ClickBench sample against external DuckDB.

Python standard library only. Queries cover representative shapes from
../ClickBench/rayforce/queries.sql, with deterministic group ordering and raw
integer dates (the Parquet source has no DATE annotation). This is a focused
correctness check, not the complete 43-query ClickBench score.
"""
import argparse
import csv
import io
import json
import math
import subprocess
import tempfile
from pathlib import Path

# name, Rayfall query body, SQL projection/body after SELECT
QUERIES = [
    ('count', 'n: (count WatchID)', 'count(*) AS n FROM hits'),
    ('aggregates', 's: (sum AdvEngineID) n: (count AdvEngineID) a: (avg ResolutionWidth)',
     'sum(AdvEngineID) AS s, count(*) AS n, avg(ResolutionWidth) AS a FROM hits'),
    ('wide_avg', 'a: (avg UserID)', 'avg(UserID) AS a FROM hits'),
    ('extrema', 'lo: (min EventDate) hi: (max EventDate)', 'min(EventDate) AS lo, max(EventDate) AS hi FROM hits'),
    ('group', 'by: AdvEngineID n: (count AdvEngineID) where: (!= AdvEngineID 0) asc: AdvEngineID',
     'AdvEngineID, count(*) AS n FROM hits WHERE AdvEngineID <> 0 GROUP BY AdvEngineID ORDER BY AdvEngineID'),
    ('distinct_group', 'by: RegionID n: (count (distinct UserID)) asc: RegionID',
     'RegionID, count(DISTINCT UserID) AS n FROM hits GROUP BY RegionID ORDER BY RegionID'),
    ('range', 'n: (count WatchID) s: (sum JavaEnable) where: (and (>= CounterID 17) (<= CounterID 20))',
     'count(*) AS n, coalesce(sum(JavaEnable),0) AS s FROM hits WHERE CounterID BETWEEN 17 AND 20'),
    ('equality', 'n: (count WatchID) where: (== CounterID 38)', 'count(*) AS n FROM hits WHERE CounterID=38'),
    ('residual', 'n: (count WatchID) where: (and (== CounterID 38) (== IsRefresh 0))',
     'count(*) AS n FROM hits WHERE CounterID=38 AND IsRefresh=0'),
    ('empty_residual', 'n: (count WatchID) where: (and (== CounterID 62) (== IsRefresh 0))',
     'count(*) AS n FROM hits WHERE CounterID=62 AND IsRefresh=0'),
    ('like', 'n: (count URL) where: (like URL "*google*")', "count(*) AS n FROM hits WHERE URL LIKE '%google%'"),
    ('topk', 'UserID: UserID desc: UserID take: 10', 'UserID FROM hits ORDER BY UserID DESC LIMIT 10'),
]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('parquet', type=Path)
    ap.add_argument('--rayforce', type=Path, default=Path('./rayforce'))
    ap.add_argument('--duckdb', default='duckdb')
    ap.add_argument('--cores', type=int, default=4)
    args = ap.parse_args()
    path = args.parquet.resolve()
    def sql(value): return "'" + str(value).replace("'", "''") + "'"
    with tempfile.TemporaryDirectory(prefix='rayforce-parquet-query-') as tmp:
        tmp = Path(tmp)
        script = tmp/'queries.rfl'
        lines = [f'(set hits (.parquet.scan {json.dumps(str(path))}))']
        for name, body, _ in QUERIES:
            lines.append(f'(.csv.write (select {{from: hits {body}}}) {json.dumps(str(tmp/(name+".csv")))})')
        lines.append('(exit 0)')
        script.write_text('\n'.join(lines)+'\n')
        subprocess.run([str(args.rayforce.resolve()),str(script),'-c',str(args.cores)], check=True)
        report = []
        for name, _, body in QUERIES:
            command = f'CREATE VIEW hits AS SELECT * FROM read_parquet({sql(path)}); SELECT {body};'
            oracle = subprocess.run([args.duckdb,'-csv','-c',command],check=True,capture_output=True,text=True).stdout
            right = list(csv.reader(io.StringIO(oracle)))
            with (tmp/(name+'.csv')).open(newline='') as f: left = list(csv.reader(f))
            if not left or not right or set(left[0]) != set(right[0]) or len(left) != len(right):
                raise RuntimeError(f'{name}: schema/row count mismatch: {left[:2]} vs {right[:2]}')
            order = [right[0].index(k) for k in left[0]]
            for row, (actual, expected) in enumerate(zip(left[1:],right[1:])):
                for col, idx in enumerate(order):
                    a, b = actual[col], expected[idx]
                    if a == b: continue
                    # Only averages use tolerance; integer results remain exact.
                    if left[0][col] == 'a' and math.isclose(float(a),float(b),rel_tol=1e-12,abs_tol=1e-12): continue
                    raise RuntimeError(f'{name}: row {row}, {left[0][col]}: {a!r} != {b!r}')
            report.append({'query':name,'rows':len(left)-1,'matched':True})
        print(json.dumps({'cores':args.cores,'queries':report},indent=2))

if __name__ == '__main__':
    main()
