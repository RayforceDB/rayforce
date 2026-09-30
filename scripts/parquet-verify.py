#!/usr/bin/env python3
"""Compare every ClickBench Parquet cell against an external DuckDB executable.

No Python packages are required. This is development tooling, not a Rayforce
build/runtime dependency. Intended for a bounded ClickBench sample (integer
and text columns); CSV text comparison is not a general floating-point oracle.
"""
import argparse
import csv
import itertools
import json
import subprocess
import tempfile
import time
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('parquet',type=Path)
    parser.add_argument('--rayforce',type=Path,default=Path('./rayforce'))
    parser.add_argument('--duckdb',default='duckdb')
    a = parser.parse_args()
    source = a.parquet.resolve(); rayforce = a.rayforce.resolve()
    def sql(s): return "'" + str(s).replace("'","''") + "'"
    with tempfile.TemporaryDirectory(prefix='rayforce-parquet-verify-') as tmp:
        tmp = Path(tmp); rf = tmp/'rayforce.csv'; oracle = tmp/'oracle.csv'; native = tmp/'native'
        script = tmp/'verify.rfl'
        script.write_text(f'''(set source {json.dumps(str(source))})
(set hits (.parquet.read source))
(.csv.write hits {json.dumps(str(rf))})
(println (.parquet.parted source {json.dumps(str(native))} 'hits))
(set p 0)
(.parquet.each source (fn [batch] (do
  (set saved (.db.splayed.get (format {json.dumps(str(native)+'/%/hits')} p)))
  (if (not (all (map (fn [c] (all (== (as 'STR (at batch c)) (as 'STR (at saved c))))) (cols batch)))) (raise "native mismatch"))
  (set p (+ p 1))
)))
(exit 0)
''')
        start = time.perf_counter()
        subprocess.run([str(rayforce),str(script)],check=True)
        rayforce_seconds = time.perf_counter()-start
        subprocess.run([a.duckdb,'-c',f"COPY (SELECT * FROM read_parquet({sql(source)})) TO {sql(oracle)} (HEADER, DELIMITER ',');"],check=True)
        csv.field_size_limit(64*1024*1024)
        count = 0
        with rf.open(newline='') as fa, oracle.open(newline='') as fb:
            left, right = csv.reader(fa), csv.reader(fb)
            columns = next(left); expected = next(right)
            if columns != expected: raise RuntimeError('schema mismatch')
            for row,(x,y) in enumerate(itertools.zip_longest(left,right)):
                if x != y:
                    if x is None or y is None: raise RuntimeError(f'row count mismatch at row {row}')
                    col = next((c for c,(u,v) in enumerate(zip(x,y)) if u != v),0)
                    raise RuntimeError(f'cell mismatch: row={row}, column={columns[col]!r}, rayforce={x[col]!r}, duckdb={y[col]!r}')
                count += 1
        print(json.dumps({'rows':count,'columns':len(columns),'matching_cells':count*len(columns),
                          'native_partitions_match':True,'read_export_convert_verify_seconds':rayforce_seconds},indent=2))

if __name__ == '__main__': main()
