#!/usr/bin/env python3
"""Exercise benchmark restart gates with real small imports and query oracles."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import struct
import subprocess
import sys
import time


def fingerprint(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, stop_after_first=None, fail=False):
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, start_new_session=True)
    if stop_after_first:
        # run.json is durable before the first timed import/query starts.
        deadline = time.monotonic()+60
        while not (stop_after_first/'run.json').exists() and process.poll() is None:
            if time.monotonic() > deadline:
                os.killpg(process.pid,signal.SIGKILL); process.communicate()
                raise RuntimeError('checkpoint startup timed out')
            time.sleep(.001)
        (stop_after_first/'stop').touch()
    try: output, _ = process.communicate(timeout=120)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid,signal.SIGKILL); output, _ = process.communicate()
        raise RuntimeError('harness self-test timed out:\n'+output)
    if (process.returncode != 0) != fail: raise RuntimeError(output)
    return output


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('source',type=Path,help='ClickBench Parquet input')
    p.add_argument('--rayforce',type=Path,required=True)
    p.add_argument('--native-verifier',type=Path,required=True)
    p.add_argument('--oracle',required=True,help='reference SQL CLI executable')
    p.add_argument('--oracle-queries', type=Path, required=True, help='reference SQL file: 43 ClickBench queries, one per line')
    p.add_argument('--work-dir',type=Path,required=True)
    a=p.parse_args(); root=a.work_dir.resolve(); root.mkdir(exist_ok=False)
    pq=root/'small.parquet'; csv=root/'small.csv'
    quote=lambda s: "'"+str(s).replace("'","''")+"'"
    subprocess.run([a.oracle,'-c',f'COPY (SELECT * FROM read_parquet({quote(a.source.resolve())}) LIMIT 1000) TO {quote(pq)} (FORMAT PARQUET, COMPRESSION SNAPPY)'],check=True)
    load=root/'loads'
    command=[sys.executable,'scripts/parquet-load-bench.py',str(pq),str(csv),'--prepare-csv',
             '--rayforce',str(a.rayforce.resolve()),'--native-verifier',str(a.native_verifier.resolve()),
             '--oracle',a.oracle,'--cores','1','2','--repeats','1','--work-dir',str(load)]
    run(command,load)
    before=json.loads((load/'results.json').read_text()); assert len(before['results'])==1
    (load/'stop').unlink()
    partial=load/'parquet-1-0.parquet-partial'; partial.mkdir(); (partial/'sentinel').write_text('retain me')
    run(command+['--resume'])
    after=json.loads((load/'results.json').read_text())
    assert len(after['results'])==4 and all(x['verified'] for x in after['results'])
    assert after['results'][0]==before['results'][0]
    assert len(list(load.glob('interrupted-*/parquet-1-0.parquet-partial/sentinel')))==1
    # Verify the verifier against damaged copies of the retained reference.
    reference=Path(after['reference']); damaged=root/'damaged-native'
    shutil.copytree(reference,damaged)
    names=sorted(p.name for p in reference.iterdir() if p.is_file() and not p.name.startswith('.'))
    verify=[str(a.native_verifier.resolve()),str(reference),str(damaged),*names]
    column=damaged/'WatchID'; original=column.read_bytes()
    changed=bytearray(original); changed[32]^=1; column.write_bytes(changed)
    assert 'value mismatch' in run(verify,fail=True)
    column.write_bytes(original)
    symbols=damaged/'.sym'; changed=bytearray(symbols.read_bytes()); offset=12
    while offset<len(changed):
        length=struct.unpack_from('<I',changed,offset)[0]; offset+=4
        if length:
            changed[offset]^=1
            break
        offset+=length
    else: raise AssertionError('small fixture has no nonempty symbols')
    symbols.write_bytes(changed)
    assert 'symbol mismatch' in run(verify,fail=True)
    print('Native verifier: rejected changed numeric value and dictionary text',flush=True)
    checkpoint=fingerprint(load/'results.json')
    run(command+['--resume'])
    assert fingerprint(load/'results.json')==checkpoint
    with csv.open('a') as f: f.write('\n')
    output=run(command+['--resume'],fail=True)
    assert 'resume identity changed' in output and fingerprint(load/'results.json')==checkpoint
    print('Load restart: retained verified cases, archived partial output, rejected changed source',flush=True)

    query=root/'queries'
    command=[sys.executable,'scripts/parquet-clickbench.py',str(pq),'--rayforce',str(a.rayforce.resolve()),
             '--oracle',a.oracle,'--oracle-queries',str(a.oracle_queries.resolve()),'--queries','1','3','--cores','1','--work-dir',str(query)]
    run(command,query)
    before=json.loads((query/'results.json').read_text()); assert len(before['results'])==1 and before['results'][0]['verified']
    old_csv=fingerprint(query/'q01.csv')
    (query/'stop').unlink(); (query/'q03.csv').write_text('interrupted output')
    run(command+['--resume'])
    after=json.loads((query/'results.json').read_text())
    assert len(after['results'])==2 and all(x['verified'] for x in after['results'])
    assert before['results'][0]==after['results'][0] and fingerprint(query/'q01.csv')==old_csv
    assert len(list(query.glob('interrupted-*/q03.csv')))==1
    checkpoint=fingerprint(query/'results.json')
    output=run(command+['--resume','--cores','2'],fail=True)
    assert 'resume identity changed' in output and fingerprint(query/'results.json')==checkpoint
    print('Query restart: retained verified answers, archived partial output, rejected changed settings',flush=True)


if __name__=='__main__': main()
