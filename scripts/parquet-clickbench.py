#!/usr/bin/env python3
"""Full ClickBench direct-Parquet census, with an external reference SQL oracle.

Standard library only. One fresh process per query; retain scripts, CSVs,
logs, query time and process RSS. This is a diagnostic census, not the official
ClickBench score. Tied/unordered LIMIT alternatives need rank and full-row
membership checks. Temporal values stay in the source's Unix units.
"""
import argparse
import csv
import datetime
import hashlib
import json
import math
import os
from pathlib import Path
from parquet_checkpoint import archive_partial, digest, open_run, save
import re
import resource
import signal
import shutil
import subprocess
import time


def file_sha256(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as f:
        for chunk in iter(lambda: f.read(8 * 1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def translate(number, ray, sql):
    changes = []
    if number in (1, 2, 5, 6, 21):
        bodies = {1: 'n: (count WatchID)',
                  2: 'n: (count AdvEngineID) where: (!= AdvEngineID 0)',
                  5: 'n: (count (distinct UserID))',
                  6: 'n: (count (distinct SearchPhrase))',
                  21: 'n: (count URL) where: (like URL "*google*")'}
        ray = '(select {from: hits ' + bodies[number] + '})'
        changes.append('express scalar count/column access through lazy select')
    if number == 19:
        ray = ray.replace('(minute EventTime)', '(% (div EventTime 60) 60)')
        sql = sql.replace('extract(minute FROM EventTime)', '(EventTime // 60) % 60')
        changes.append('minute from explicit Unix seconds')
    if number in (25, 27):
        sql = sql.replace('SELECT SearchPhrase FROM', 'SELECT SearchPhrase, EventTime FROM')
        changes.append('retain EventTime projected by the ClickBench Rayforce query')
    if number == 29:
        # Match the SQL regexp exactly: its dot does not match a newline in
        # the path, the protocol must be http(s), and the hostname is nonempty.
        # For www./ the optional prefix must backtrack, leaving host www.
        key = ('(let p (str-find Referer "://") '
               '(let s (substr Referer (+ p 4) -1) '
               '(let sl (str-find s "/") '
               '(if (and (or (== (substr Referer 1 7) "http://") '
               '(== (substr Referer 1 8) "https://")) (> sl 0) '
               '(nil? (str-find (substr s (+ sl 2) -1) "\\n"))) '
               '(if (and (== (str-find s "www.") 0) (> sl 4)) '
               '(substr s 5 (- sl 4)) (substr s 1 sl)) Referer))))')
        start = ray.index('by: ') + len('by: ')
        end = ray.index(' l: ', start)
        ray = ray[:start] + key + ray[end:]
        changes.append('match SQL URL regexp protocol, nonempty host and newline semantics')
    if number == 36:
        sql = sql.replace('SELECT ClientIP, ClientIP - 1, ClientIP - 2, ClientIP - 3, COUNT(*) AS c',
                          'SELECT ClientIP, COUNT(*) AS c, ClientIP - 1, ClientIP - 2, ClientIP - 3')
        changes.append('align computed-group-key output column order')
    if number >= 37:
        def days(match):
            return str((datetime.date.fromisoformat(match[0].strip("'").replace('.', '-')) -
                        datetime.date(1970, 1, 1)).days)
        ray = re.sub(r'2013\.07\.\d\d', days, ray)
        sql = re.sub(r"'2013-07-\d\d'", days, sql)
        changes.append('date predicates in explicit Unix days')
    if number == 43:
        ray = ray.replace('(xbar EventTime 60000000000)', '(xbar EventTime 60)')
        sql = sql.replace("DATE_TRUNC('minute', EventTime)", '(EventTime // 60) * 60')
        changes.append('minute buckets in explicit Unix seconds')
    return ray, sql, changes


def run(command, log, timeout, address_space_gib=0):
    def limits():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        if address_space_gib:
            size = address_space_gib * 1024**3
            resource.setrlimit(resource.RLIMIT_AS, (size, size))
    start = time.monotonic()
    with log.open('w') as output:
        process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT,
                                   start_new_session=True, preexec_fn=limits)
        try:
            code = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()
            return {'status': 'timeout', 'wall_seconds': time.monotonic()-start}
    return {'status': 'executed' if code == 0 else 'execution_failed',
            'returncode': code, 'wall_seconds': time.monotonic()-start}


def compare(actual, expected):
    # Oracle CSV header spellings differ; compare positional output. Averages
    # alone accept rounding tolerance; strings that look numeric remain strings.
    if len(actual) != len(expected):
        return False, 'row count differs'
    for i, (a, b) in enumerate(zip(actual, expected)):
        if len(a) != len(b):
            return False, 'column count differs'
        for c, (x, y) in enumerate(zip(a, b)):
            if x == y:
                continue
            if isinstance(y, float):
                try:
                    if math.isclose(float(x), y, rel_tol=1e-12, abs_tol=1e-12):
                        continue
                except ValueError:
                    pass
            elif y is not None and x == str(y):
                continue
            return False, f'row {i}, column {c}: {x!r} != {y!r}'
    return True, None


def verify_ties(a, stem, ray, sql, setup, headers, actual, expected):
    """Check rank keys, full-row membership, and multiplicity independently.

    Never accept a different result merely because LIMIT admits ties. The
    expected rank sequence includes OFFSET. Unordered Q18 needs membership.
    Duplicate rows may be valid (e.g. repeated SearchPhrase/EventTime pairs).
    Require enough matching source rows for every repeated output tuple.
    """
    if len(actual) != len(expected) or not actual:
        return False
    # Distinct near-equal float tuples must not both claim the same oracle row.
    if any(isinstance(x, float) for r in expected for x in r) and len(set(map(tuple, actual))) != len(actual):
        return False
    order = re.findall(r'(?:asc|desc):\s*(\[[^]]+\]|\w+)', ray)
    if order:
        keys = order[-1].strip('[]').split()
        if any(k not in headers for k in keys):
            return False
        indices = [headers.index(k) for k in keys]
        if not compare([[r[i] for i in indices] for r in actual],
                       [[r[i] for i in indices] for r in expected])[0]:
            return False
    elif 'ORDER BY' in sql:
        return False
    base = re.split(r'\s+(?:ORDER BY|LIMIT)\s+', sql, flags=re.I)[0].rstrip(';')
    names = [f'c{i}' for i in range(len(headers))]
    comparisons = []
    for i, name in enumerate(names):
        if isinstance(expected[0][i], float):
            comparisons.append(f'abs(try_cast(a.{name} AS DOUBLE)-r.{name}) <= '
                               f'1e-12 * greatest(1, abs(r.{name}))')
        else:
            comparisons.append(f"coalesce(a.{name}, '') = coalesce(cast(r.{name} AS VARCHAR), '')")
    path = "'" + str(stem.with_suffix('.csv')).replace("'", "''") + "'"
    query = (setup + f'WITH r({",".join(names)}) AS ({base}), '
             f'a0({",".join(names)}) AS (SELECT * FROM read_csv({path}, header=true, all_varchar=true)), '
             f'a AS (SELECT *, count(*) OVER (PARTITION BY {",".join(names)}) AS copies FROM a0) '
             'SELECT count(*) AS matches FROM a WHERE '
             f'(SELECT count(*) FROM r WHERE {" AND ".join(comparisons)}) >= a.copies;')
    stem.with_suffix('.ties.sql').write_text(query+'\n')
    output = stem.with_suffix('.ties.json')
    status = run([a.oracle, '-json', '-c', query], output, a.timeout)
    if status['status'] != 'executed':
        return False
    return json.loads(output.read_text())[0]['matches'] == len(actual)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('parquet', type=Path)
    p.add_argument('--rayforce', type=Path, default=Path('./rayforce'))
    p.add_argument('--clickbench', type=Path, default=Path('../ClickBench'))
    p.add_argument('--oracle', required=True, help='reference SQL CLI executable')
    p.add_argument('--oracle-queries', type=Path, required=True, help='reference SQL file: 43 ClickBench queries, one per line')
    p.add_argument('--cores', type=int, default=os.cpu_count())
    p.add_argument('--timeout', type=int, default=180)
    p.add_argument('--address-space-gib', type=int, default=0,
                   help='optional process virtual-memory ceiling, explicitly recorded')
    p.add_argument('--queries', type=int, nargs='+', default=list(range(1, 44)))
    p.add_argument('--symbol-text', action='store_true',
                   help='decode text into private SYM dictionaries; keep temporal source integers')
    p.add_argument('--work-dir', type=Path, required=True)
    p.add_argument('--resume', action='store_true', help='continue an unchanged checkpointed census')
    a = p.parse_args()
    if a.cores < 1 or a.timeout < 1 or any(q not in range(1, 44) for q in a.queries):
        p.error('invalid cores, timeout or query numbers')
    if len(set(a.queries)) != len(a.queries): p.error('query numbers must not contain duplicates')
    root = a.work_dir.resolve()
    if a.resume:
        if not root.is_dir(): p.error('--resume requires an existing work directory')
    else: root.mkdir(exist_ok=False)
    ray_queries = (a.clickbench/'rayforce/queries.sql').read_text().splitlines()
    sql_queries = a.oracle_queries.read_text().splitlines()
    if len(ray_queries) != 43 or len(sql_queries) != 43:
        raise RuntimeError('expected 43 one-line queries per ClickBench adapter')
    options = ''
    source_types = None
    if a.symbol_text:
        schema = (a.clickbench/'rayforce/create.rfl').read_text()
        match = re.search(r'\(set hits-types \[([^]]+)\]', schema, re.S)
        if not match: raise RuntimeError('cannot find ClickBench types')
        source_types = ['I64' if t == 'TIMESTAMP' else 'I32' if t == 'DATE' else t
                        for t in match.group(1).split()]
        options = '{types: [' + ' '.join(source_types) + ']} '
    report = {'input': str(a.parquet.resolve()), 'input_bytes': a.parquet.stat().st_size,
              'input_sha256': file_sha256(a.parquet),
              'binary_sha256': hashlib.sha256(a.rayforce.read_bytes()).hexdigest(),
              'harness_sha256': file_sha256(__file__),
              'cores': a.cores, 'timeout_seconds': a.timeout,
              'address_space_gib': a.address_space_gib, 'cache': 'OS-managed',
              'source_types': source_types,
              'method': 'fresh process per query; query timing excludes result CSV; not official ClickBench score',
              'results': []}
    identity = {k:v for k,v in report.items() if k != 'results'}
    identity.update(queries=a.queries,ray_queries_sha256=digest(a.clickbench/'rayforce/queries.sql'),
                    sql_queries_sha256=digest(a.oracle_queries),
                    checkpoint_sha256=digest(Path(__file__).with_name('parquet_checkpoint.py')),
                    oracle_sha256=digest(shutil.which(a.oracle) or a.oracle))
    previous = open_run(root,a.resume,identity)
    if previous: report = previous
    completed = {x['query'] for x in report['results']}
    if len(completed) != len(report['results']): raise RuntimeError('duplicate query checkpoints')
    if not completed <= set(a.queries): raise RuntimeError('unexpected query checkpoint')
    for number in a.queries:
        if number in completed: continue
        ray, sql, changes = translate(number, ray_queries[number-1], sql_queries[number-1])
        stem = root/f'q{number:02d}'
        archive_partial(root,list(root.glob(stem.name+'.*')))
        output = stem.with_suffix('.csv')
        script = stem.with_suffix('.rfl')
        script.write_text(f'(set hits (.parquet.scan {options}{json.dumps(str(a.parquet.resolve()))}))\n'
                          f'(println (timeit (set result {ray})))\n'
                          f'(.csv.write result {json.dumps(str(output))})\n(exit 0)\n')
        timing = stem.with_suffix('.time')
        print(f'Q{number:02d} Rayforce', flush=True)
        result = run(['/usr/bin/time', '-f', '%e %M %U %S', '-o', str(timing),
                      str(a.rayforce.resolve()), '-c', str(a.cores), str(script)],
                     stem.with_suffix('.log'), a.timeout, a.address_space_gib)
        result.update(query=number, translations=changes, rayfall=ray, sql=sql, verified=False)
        log_text = stem.with_suffix('.log').read_text()
        try:
            result['query_ms'] = float(log_text.splitlines()[0])
        except (ValueError, IndexError):
            result['diagnostic'] = log_text[-2000:]
        if timing.exists():
            lines = timing.read_text().splitlines()
            tokens = lines[-1].split() if lines else []
            if len(tokens) == 4:
                result.update(process_seconds=float(tokens[0]), peak_rss_kib=int(tokens[1]),
                              user_seconds=float(tokens[2]), system_seconds=float(tokens[3]))
        if result['status'] == 'executed' and output.exists():
            print(f'Q{number:02d} reference SQL engine', flush=True)
            source = "'" + str(a.parquet.resolve()).replace("'", "''") + "'"
            setup = f"SET threads={a.cores}; SET memory_limit='32GB'; CREATE VIEW hits AS SELECT * FROM read_parquet({source}); "
            oracle_sql = setup + sql
            stem.with_suffix('.sql').write_text(oracle_sql+'\n')
            oracle_path = stem.with_suffix('.oracle.json')
            oracle = run([a.oracle, '-json', '-c', oracle_sql], oracle_path, a.timeout)
            result['oracle'] = oracle
            if oracle['status'] == 'executed':
                rows = json.loads(oracle_path.read_text().strip() or '[]')
                expected = [list(row.values()) for row in rows]
                with output.open(newline='') as f:
                    reader = csv.reader(f); headers = next(reader); actual = list(reader)
                matched, mismatch = compare(actual, expected)
                if not matched and ('LIMIT' in sql or number == 8):
                    matched = verify_ties(a, stem, ray, sql, setup, headers, actual, expected)
                    if matched:
                        result['tie_validation'] = 'rank sequence, full-row oracle membership and multiplicity'
                        result['initial_order_difference'] = mismatch
                        mismatch = None
                result.update(verified=matched, rows=len(actual), mismatch=mismatch)
                if matched:
                    result['status'] = 'verified'
                else:
                    # A diagnostic flag, not an assertion that the result is
                    # valid: tie-aware membership validation is still required.
                    result['status'] = 'mismatch'
                    result['ordering_may_be_ambiguous'] = 'LIMIT' in sql or number == 8
            else:
                result['status'] = 'oracle_failed'
        elif result['status'] == 'executed':
            result['status'] = 'missing_output'
        report['results'].append(result)
        save(root/'results.json',report)
        print(f'Q{number:02d}: {result["status"]}', flush=True)
        if (root/'stop').exists():
            break


if __name__ == '__main__':
    main()
