#!/usr/bin/env python3
"""Compare ClickBench's CSV load with Parquet -> the identical native schema.

Standard library only. Inputs must contain the same rows in the same order.
CSV is headerless, with ISO dates/timestamps, as in ClickBench/rayforce/load.rfl.
The published Athena Parquet uses unannotated Unix seconds/days; these units
are explicit in the import schema. Download/export are outside load timing.
Every measured load includes process startup, native indexes, reopening, and
fsync of all output files/directories. Inputs use the OS page cache (no privileged
cache drop). Each output is checked against the first CSV import, then removed.
"""
import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import statistics
import subprocess
import tempfile
import time
from pathlib import Path
from parquet_checkpoint import archive_partial, digest, file_identity, open_run, save


def sync_tree(root):
    for parent, dirs, files in os.walk(root, topdown=False):
        for name in files:
            fd = os.open(Path(parent)/name, os.O_RDONLY)
            try: os.fsync(fd)
            finally: os.close(fd)
        fd = os.open(parent, os.O_RDONLY)
        try: os.fsync(fd)
        finally: os.close(fd)
    fd = os.open(root.parent, os.O_RDONLY)
    try: os.fsync(fd)
    finally: os.close(fd)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('parquet', type=Path); p.add_argument('csv', type=Path)
    p.add_argument('--rayforce', type=Path, default=Path('./rayforce'))
    p.add_argument('--clickbench', type=Path, default=Path('../ClickBench'))
    p.add_argument('--cores', type=int, nargs='+', default=[1,8,28])
    p.add_argument('--repeats', type=int, default=3)
    p.add_argument('--reference', type=Path, help='existing verified native CSV reference; retained')
    p.add_argument('--kinds', choices=['csv','parquet'], nargs='+', default=['csv','parquet'])
    p.add_argument('--prepare-csv', action='store_true', help='export identical rows with external reference SQL engine before timing; CSV must not exist')
    p.add_argument('--oracle', help='reference SQL CLI executable')
    p.add_argument('--native-verifier', type=Path, help='bounded-memory verifier built from bench/parquet_load/verify-native.c')
    p.add_argument('--layout', choices=['splayed','parted'], default='splayed')
    p.add_argument('--partition-rows', type=int, default=65536,
                   help='maximum rows per partition (Parquet also splits at row-group boundaries)')
    p.add_argument('--work-dir', type=Path, required=True)
    p.add_argument('--resume', action='store_true', help='continue an unchanged checkpointed run; retain completed verified cases')
    a = p.parse_args()
    if a.prepare_csv and not a.resume and not a.oracle: p.error('--prepare-csv requires --oracle')
    if a.repeats < 1 or any(c < 1 for c in a.cores): p.error('positive repeats/cores required')
    if len(set(a.cores)) != len(a.cores) or len(set(a.kinds)) != len(a.kinds): p.error('cores and kinds must not contain duplicates')
    if not 1 <= a.partition_rows <= 1048576: p.error('partition rows must be 1..1048576')
    if a.layout == 'parted' and (not a.reference or not a.native_verifier):
        p.error('parted requires --reference (verified splayed) and --native-verifier')
    root = a.work_dir.resolve()
    if a.resume:
        if not root.is_dir(): p.error('--resume requires an existing work directory')
    else: root.mkdir(exist_ok=False)
    binary = str(a.rayforce.resolve()); create = (a.clickbench/'rayforce/create.rfl').resolve()
    # Read the actual benchmark schema, not a separately maintained type list.
    schema = create.read_text(); m = re.search(r'\(set hits-types \[([^]]+)\]',schema,re.S)
    if not m: raise RuntimeError('cannot find ClickBench type vector')
    types = m.group(1).split()
    names = re.search(r'\(set hits-names \[([^]]+)\]',schema,re.S).group(1).split()
    parquet_types = ['UNIX_SECONDS' if t == 'TIMESTAMP' else 'UNIX_DATE' if t == 'DATE' else t for t in types]
    quote = lambda x: json.dumps(str(x))
    if a.prepare_csv and not a.resume:
        if a.csv.exists(): raise FileExistsError(a.csv)
        names = re.search(r'\(set hits-names \[([^]]+)\]',schema,re.S).group(1).split()
        ident = lambda x: '"'+x.replace('"','""')+'"'
        sqlstr = lambda x: "'"+str(x).replace("'","''")+"'"
        replacements = []
        for name, typ in zip(names,types):
            col = ident(name)
            if typ == 'TIMESTAMP': replacements.append(f"strftime(to_timestamp({col}), '%Y-%m-%d %H:%M:%S') AS {col}")
            if typ == 'DATE': replacements.append(f"strftime(DATE '1970-01-01' + {col}, '%Y-%m-%d') AS {col}")
        sql = ("SET TimeZone='UTC'; SET threads=8; COPY (SELECT * REPLACE ("+', '.join(replacements)+
               f") FROM read_parquet({sqlstr(a.parquet.resolve())})) TO {sqlstr(a.csv.resolve())} (HEADER false, DELIMITER ',');")
        (root/'prepare.sql').write_text(sql+'\n')
        subprocess.run([a.oracle,'-c',sql],check=True)
    reference = a.reference.resolve() if a.reference else None
    identity = {'parquet':file_identity(a.parquet),'csv':file_identity(a.csv),
                'schema_sha256':digest(create),'binary_sha256':digest(binary),
                'harness_sha256':digest(__file__),'checkpoint_sha256':digest(Path(__file__).with_name('parquet_checkpoint.py')),
                'layout':a.layout,'partition_rows':a.partition_rows,'cores':a.cores,
                'repeats':a.repeats,'kinds':a.kinds,'reference':str(reference) if reference else None,
                'verifier_sha256':digest(a.native_verifier) if a.native_verifier else None}
    if reference:
        identity['reference_files'] = [file_identity(reference/name) for name in ['.d',*names] if (reference/name).exists()]
        if (reference/'.sym').exists(): identity['reference_files'].append(file_identity(reference/'.sym'))
    previous = open_run(root,a.resume,identity)
    results = previous['results'] if previous else []
    expected_rows = results[0]['rows'] if results else None
    if previous and not reference: reference = Path(previous['reference'])
    reference_identity = lambda: [file_identity(reference/name) for name in ['.d','.sym',*names] if (reference/name).exists()]
    if previous and previous.get('reference_identity') != reference_identity():
        raise RuntimeError('resume reference files changed')
    completed = {(x['kind'],x['cores'],x['repeat']) for x in results if x['verified']}
    if len(completed) != len(results): raise RuntimeError('invalid or duplicate checkpoint cases')
    planned = {(k,c,r) for k in a.kinds for c in a.cores for r in range(a.repeats)}
    if not completed <= planned: raise RuntimeError('unexpected checkpoint case')
    report = previous
    if a.kinds != ['csv','parquet'] and reference is None:
        p.error('--reference is required when measuring only one input format')
    for cores in a.cores:
        for repeat in range(a.repeats):
            for kind in (a.kinds if repeat % 2 == 0 else list(reversed(a.kinds))):
                if (kind,cores,repeat) in completed: continue
                dest = root/f'{kind}-{cores}-{repeat}'; script = root/'load.rfl'; timing = root/'process.time'
                archive_partial(root,[dest,Path(str(dest)+'.parquet-partial'),Path(str(dest)+'.csv-partial'),
                                      root/f'{kind}-{cores}-{repeat}.log',root/f'{kind}-{cores}-{repeat}.verify.log'])
                if a.layout == 'parted':
                    if kind == 'csv':
                        expr = f"(.csv.parted hits-names hits-types {quote(a.csv.resolve())} {a.partition_rows} {quote(dest)} 'hits)"
                    else:
                        expr = f"(.parquet.parted {{types: [{' '.join(parquet_types)}] rows: {a.partition_rows}}} {quote(a.parquet.resolve())} {quote(dest)} 'hits)"
                    expr += f"\n(set hits (.db.parted.get {quote(dest)} 'hits))"
                elif kind == 'csv':
                    expr = f'(set hits (.csv.splayed hits-names hits-types {quote(a.csv.resolve())} {quote(dest)}))'
                else:
                    expr = f'(.parquet.splayed [{" ".join(parquet_types)}] {quote(a.parquet.resolve())} {quote(dest)})\n(set hits (.db.splayed.get {quote(dest)}))'
                script.write_text(f'(load {quote(create)})\n{expr}\n(println (count hits))\n(exit 0)\n')
                print(f'loading {kind} cores={cores} repeat={repeat}',flush=True)
                start = time.perf_counter()
                run = subprocess.run(['/usr/bin/time','-f','%e %M %U %S %F %R %I %O','-o',str(timing),binary,'-c',str(cores),str(script)],capture_output=True,text=True)
                (root/f'{kind}-{cores}-{repeat}.log').write_text(run.stdout+run.stderr)
                if run.returncode: raise RuntimeError(f'load failed: {run.stderr[-2000:]}')
                rows = int(run.stdout.strip()); sync_tree(dest)
                elapsed = time.perf_counter()-start
                process, rss, user, system, major, minor, reads, writes = timing.read_text().split()
                if expected_rows is None: expected_rows = rows
                if rows != expected_rows: raise RuntimeError('row count mismatch')
                print(f'loaded {kind}: {rows} rows in {elapsed:.3f}s; verifying',flush=True)
                size = sum(f.stat().st_size for f in dest.rglob('*') if f.is_file())
                if reference is None:
                    reference = dest
                else:
                    verify = root/'verify.rfl'
                    verify.write_text(f'''(set left (.db.splayed.get {quote(reference)}))
(set right (.db.splayed.get {quote(dest)}))
(if (not (all (== (cols left) (cols right)))) (raise "column names mismatch"))
(map (fn [c] (do
  (set x (at left c)) (set y (at right c))
  (if (!= (type x) (type y)) (raise "column type mismatch"))
  (if (not (all (== x y))) (do (println c) (raise "column mismatch")))
)) (cols left))
(println "VERIFIED")
(exit 0)
''')
                    command = ([str(a.native_verifier.resolve()),*(['--parted','hits'] if a.layout == 'parted' else []),str(reference),str(dest),*names] if a.native_verifier else
                               [binary,'-c',str(cores),str(verify)])
                    verification_log = root/f'{kind}-{cores}-{repeat}.verify.log'
                    with verification_log.open('w') as output:
                        check = subprocess.run(command,stdout=output,stderr=subprocess.STDOUT,text=True)
                    checked = verification_log.read_text()
                    if check.returncode or 'VERIFIED' not in checked: raise RuntimeError(f'output verification failed: {checked[-2000:]}')
                    shutil.rmtree(dest)
                result = {'kind':kind,'layout':a.layout,'cores':cores,'repeat':repeat,'rows':rows,'columns':len(types),
                          'durable_load_seconds':elapsed,'process_seconds':float(process),'peak_rss_kib':int(rss),'native_bytes':size,'verified':True,
                          'process_user_seconds':float(user),'process_system_seconds':float(system),
                          'major_faults':int(major),'minor_faults':int(minor),
                          'input_blocks':int(reads),'output_blocks':int(writes)}
                results.append(result); print(json.dumps(result),flush=True)
                report = {'parquet':str(a.parquet.resolve()),'csv':str(a.csv.resolve()),'schema':str(create),'cpu':platform.processor(),'logical_cpus':os.cpu_count(),
                          'layout':a.layout,'binary_sha256':hashlib.sha256(Path(binary).read_bytes()).hexdigest(),
                          'reference':str(reference),'harness_sha256':identity['harness_sha256'],
                          'reference_identity':reference_identity(),
                          'partitioning':f'source order, at most {a.partition_rows} rows; Parquet also breaks at row-group boundaries' if a.layout == 'parted' else None,
                          'input_bytes':{'csv':a.csv.stat().st_size,'parquet':a.parquet.stat().st_size},
                          'page_cache':'OS-managed; no cache eviction','timing':'process + native indexes + reopen + output fsync',
                          'results':results,'medians':[]}
                for c in a.cores:
                    entries = {k:[v['durable_load_seconds'] for v in results if v['cores']==c and v['kind']==k] for k in ('csv','parquet')}
                    if all(entries.values()):
                        med = {k:statistics.median(v) for k,v in entries.items()}
                        report['medians'].append({'cores':c,**med,'csv_over_parquet':med['csv']/med['parquet']})
                save(root/'results.json',report)
                if (root/'stop').exists():
                    print('Stopped after verified case: work directory contains stop file',flush=True)
                    return
    print(json.dumps(report['medians'],indent=2))

if __name__ == '__main__': main()
