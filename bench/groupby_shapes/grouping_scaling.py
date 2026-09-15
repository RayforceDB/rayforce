#!/usr/bin/env python3
"""Portable grouping-family scaling matrix, with typed baseline comparisons."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import tempfile

from scaling_common import compare_csv, profile_phases

# (preparation, select fields, keys, optional selection)
CASES = {}
for kind in ("BOOL", "U8", "I16", "I32", "I64", "F32", "F64", "TIME"):
    source = "(> v 0)" if kind == "BOOL" else "(as 'U8 (+ v 50))" if kind == "U8" else f"(as '{kind} v)"
    # F32 conversion is admitted by the expression materializer.
    prep = f"(set t (select {{from:t i:i k:k j:j v:{source} f:f w:w}}))"
    CASES[f"sum-{kind.lower()}"] = (prep, "s:(sum v)", "k", "")
    CASES[f"extrema-{kind.lower()}"] = (prep, "s:(min v) hi:(max v)", "k", "")
CASES.update({
    "count": ("", "s:(count v)", "k", ""),
    "statistics": ("", "s:(avg f) v:(var f) sd:(stddev f)", "k", ""),
    "binary": ("", "s:(wsum f w) a:(wavg f w) p:(pearson_corr f w) c:(cov f w)", "k", ""),
    "truth": ("", "s:(all v) a:(any v)", "k", ""),
    "product": ("(set t (update {from:t v:(+ (% i 2) -1)}))", "s:(prod v)", "k", ""),
    "median": ("", "s:(med v)", "k", ""),
    "quantile": ("", "s:(quantile v 0.25)", "k", ""),
    "mode": ("", "s:(mode v)", "k", ""),
    "top-bottom": ("", "s:(top v 3) b:(bot v 3)", "k", ""),
    "first-last": ("", "s:(first v) z:(last v)", "k", ""),
    "mixed": ("", "s:(sum v) m:(med v) a:(avg f)", "k", ""),
    "distinct": ("", "s:(count (distinct v))", "k", ""),
    "composite": ("", "s:(sum v) m:(min v)", "[k j]", ""),
    "selected": ("", "s:(sum v) m:(min v)", "k", "where:(> v 25)"),
    "selected-indexed": ("", "s:(sum v) m:(med v)", "[k j]", "where:(> v 25)"),
    "sparse": ("(set t (select {from:t i:i k:(* (as 'I64 k) 1000000007) j:j v:v f:f w:w}))", "s:(sum v)", "k", ""),
    "clustered": ("(set t (update {from:t k:(as 'I32 (/ i 8))}))", "s:(sum v)", "k", ""),
    "skew": ("(set t (update {from:t k:(as 'I32 0N) where:(< (% i 10) 4)}))", "s:(sum v)", "k", ""),
    "skew-indexed": ("(set t (update {from:t k:(as 'I32 0N) where:(< (% i 10) 4)}))", "s:(sum v) m:(med v)", "k", ""),
    "float-key": ("(set t (select {from:t i:i k:(as 'F64 k) j:j v:v f:f w:w}))", "s:(sum v)", "k", ""),
    "float-indexed": ("(set t (select {from:t i:i k:(as 'F64 k) j:j v:v f:f w:w}))", "s:(sum v) m:(med v)", "k", ""),
    "symbol-key": ("(set t (table [i k j v f w] (list i (at ['a 'b 'c 'd] (% i 4)) (at t 'j) (at t 'v) (at t 'f) (at t 'w))))", "s:(sum v)", "k", ""),
    "string-key": ('(set t (table [i k j v f w] (list i (at ["pooled long alpha value" "beta" "gamma" "delta"] (% i 4)) (at t \'j) (at t \'v) (at t \'f) (at t \'w))))', "s:(sum v)", "k", ""),
    "string-indexed": ('(set t (table [i k j v f w] (list i (at ["pooled long alpha value" "beta" "gamma" "delta"] (% i 4)) (at t \'j) (at t \'v) (at t \'f) (at t \'w))))', "s:(sum v) m:(med v)", "k", ""),
    "wide-extrema": ('(set t (table [i k j v f w text] (list i (at t \'k) (at t \'j) (at t \'v) (at t \'f) (at t \'w) (at ["pooled long alpha value" "beta" "gamma" "delta"] (% i 4)))))', "s:(min text) z:(max text)", "k", ""),
})
for kind in ("DATE", "TIMESTAMP"):
    CASES[f"extrema-{kind.lower()}"] = (
        f"(set t (select {{from:t i:i k:k j:j v:(as '{kind} v) f:f w:w}}))",
        "s:(min v) hi:(max v)", "k", "")
for name, fields in (("wide-mode", "s:(mode text)"),
                     ("wide-top-bottom", "s:(top text 3) b:(bot text 3)")):
    CASES[name] = (CASES["wide-extrema"][0], fields, "k", "")
for name, value in (("extrema-ascending", "i"), ("extrema-descending", "(- 0 i)")):
    CASES[name] = (f"(set t (select {{from:t i:i k:k j:j v:(as 'I32 {value}) f:f w:w}}))",
                   "s:(min v) hi:(max v)", "k", "")

CASES["guid-key"] = (
    "(set g (as 'GUID (list \"00000000-0000-0000-0000-000000000001\" \"00000000-0000-0000-0000-000000000002\" \"00000000-0000-0000-0000-000000000003\" \"\")))\n"
    "(set t (table [i k j v f w] (list i (at g (% i 4)) (at t 'j) (at t 'v) (at t 'f) (at t 'w))))",
    "s:(sum v) m:(med v)", "k", "")
CASES["list-key"] = (
    "(set g (list [1 2] [3 4] ['a 'b] [\"pooled list string\" \"x\"]))\n"
    "(set t (table [i k j v f w] (list i (at g (% i 4)) (at t 'j) (at t 'v) (at t 'f) (at t 'w))))",
    "s:(sum v) m:(med v)", "k", "")
CASES["unique-keys"] = ("(set t (update {from:t k:(as 'I32 i)}))", "s:(sum v)", "k", "")
CASES["nonnull"] = ("(set t (update {from:t v:(- (% i 101) 50)}))", "s:(sum v) a:(avg v)", "k", "")

for direction, value in (("ascending", "i"), ("descending", "(- 0 i)")):
    CASES[f"hot-min-{direction}"] = (
        f"(set t (select {{from:t i:i k:k j:j v:(as 'I32 {value}) f:f w:w}}))\n"
        "(set t (update {from:t k:(as 'I32 0) where:(== (% i 2) 0)}))",
        "s:(min v)", "k", "")
CASES["all-null-key"] = ("(set t (update {from:t k:(as 'I32 0N)}))", "s:(sum v) a:(avg v)", "k", "")

CASES["distinct-skew"] = (
    "(set t (update {from:t k:(as 'I32 0N) v:0 where:(< (% i 10) 4)}))",
    "s:(count (distinct v))", "k", "")
CASES["distinct-correlated"] = ("", "s:(count (distinct k))", "k", "")
CASES["symbol-extrema"] = (CASES["wide-extrema"][0].replace('["pooled long alpha value" "beta" "gamma" "delta"]', "['alpha 'beta 'gamma 'delta]"), "s:(min text) z:(max text)", "k", "")
CASES["symbol-top-bottom"] = (CASES["symbol-extrema"][0], "s:(top text 3) z:(bot text 3)", "k", "")
CASES["distinct-string-values"] = (CASES["wide-extrema"][0], "s:(count (distinct text))", "k", "")
for kind in ("string", "guid", "list"):
    CASES[f"distinct-{kind}-keys"] = (CASES[f"{kind}-key"][0], "s:(count (distinct v))", "k", "")
    CASES[f"mixed-distinct-{kind}-keys"] = (
        CASES[f"{kind}-key"][0], "s:(count (distinct v)) total:(sum v)", "k", "")
CASES["mixed-distinct-string-values"] = (
    CASES["wide-extrema"][0], "s:(count (distinct text)) total:(sum v)", "k", "")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", type=Path, required=True)
    ap.add_argument("--baseline", type=Path)
    ap.add_argument("--baseline-skip-cases", default="",
                    help="cases with a recorded baseline failure; current results still checked across workers")
    ap.add_argument("--workers", default="1,2,4,8,16,default")
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--rows", type=int, default=1000000)
    ap.add_argument("--cases", default=",".join(CASES))
    ap.add_argument("--profile", action="store_true")
    ap.add_argument("--stop-file", type=Path, help="stop between processes when this file exists")
    ap.add_argument("--output", type=Path, required=True)
    args = ap.parse_args()
    cases, workers = args.cases.split(","), args.workers.split(",")
    baseline_skip = set(filter(None, args.baseline_skip_cases.split(",")))
    if any(c not in CASES for c in cases) or args.rounds < 1 or args.rows < 1:
        ap.error("invalid cases, rounds or rows")
    if any(w != "default" and (not w.isdecimal() or int(w) < 1) for w in workers):
        ap.error("worker counts must be positive or default")
    binaries = [("current", args.binary.resolve())]
    if args.baseline:
        binaries.insert(0, ("baseline", args.baseline.resolve()))
    env = os.environ.copy()
    env.pop("RAYFORCE_CORES", None)
    log_dir = args.output.with_suffix(".runs")
    log_dir.mkdir(parents=True, exist_ok=True)
    if baseline_skip - CASES.keys():
        ap.error("unknown baseline skip case")
    matrix = [(case, worker, name, binary) for case in cases for worker in workers for name, binary in binaries
              if not (name == "baseline" and case in baseline_skip)]
    records = []
    with tempfile.TemporaryDirectory(prefix="rayforce-grouping-") as temp:
        directory = Path(temp)
        script, result, schema, rss, input_schema = (directory / f for f in ("query.rfl", "result.csv", "schema.csv", "rss", "input-schema.csv"))
        references = set()
        for round_number in range(1, args.rounds + 1):
            for case, worker, name, binary in matrix if round_number % 2 else reversed(matrix):
                if args.stop_file and args.stop_file.exists():
                    print(f"Stopped between processes; {len(records)} verified runs retained in {args.output}")
                    return
                prep, fields, keys, selection = CASES[case]
                key_list = "['k 'j]" if keys == "[k j]" else "['k]"
                query = f"(select {{from:t by:{keys} {fields} {selection}}})"
                setup = f"""(set i (til {args.rows}))
(set v (- (% i 101) 50))
(set t (table [i k j v f w] (list i (as 'I32 (% (* i 17) 65536)) (as 'I32 (% i 7)) v (as 'F64 v) (as 'F64 (+ (% i 31) 1)))))
(set t (update {{from:t v:0N where:(== (% i 97) 0)}}))
{prep}
"""
                text = setup + f"(println (count t))\n(println (timeit (set r {query})))\n(println (count r))\n"
                text += (f"(println (timeit {query}))\n" * 5)
                text += f"(set ordered (xasc r {key_list}))\n(.csv.write ordered {json.dumps(str(result))})\n"
                text += "(.csv.write (table [column kind] (list (key ordered) (map (fn [c] (at (meta (at ordered c)) 'type)) (key ordered)))) " + json.dumps(str(schema)) + ")\n"
                text += "(.csv.write (table [column kind] (list (key t) (map (fn [c] (at (meta (at t c)) 'type)) (key t)))) " + json.dumps(str(input_schema)) + ")\n"
                script.write_text(text)
                for p in (result, schema, rss):
                    p.unlink(missing_ok=True)
                command = ["/usr/bin/time", "-f", "%M", "-o", str(rss), str(binary), str(script)]
                if worker != "default":
                    command += ["-c", worker]
                if args.profile:
                    command += ["-t", "1"]
                run = subprocess.run(command, env=env, capture_output=True, text=True)
                (log_dir / f"{case}-{worker}-{name}-{round_number}.log").write_text(run.stdout + run.stderr)
                if run.returncode:
                    raise RuntimeError(f"{case} failed: {run.stdout}\n{run.stderr}")
                values = [float(line) for line in run.stdout.splitlines() if re.fullmatch(r"[0-9]+(?:\.[0-9]+)?", line.strip())]
                if len(values) < 8 or not result.is_file() or not schema.is_file():
                    raise RuntimeError(f"Incomplete {case}: {run.stdout}\n{run.stderr}")
                reference, ref_schema = directory / f"{case}.csv", directory / f"{case}-schema.csv"
                if case not in references:
                    shutil.copyfile(result, reference)
                    shutil.copyfile(schema, ref_schema)
                    references.add(case)
                if schema.read_bytes() != ref_schema.read_bytes():
                    raise RuntimeError(f"Result types differ for {case}")
                try:
                    compare_csv(reference, result, schema, key_names=("k", "j"))
                except RuntimeError:
                    shutil.copyfile(reference, log_dir / "mismatch-reference.csv")
                    shutil.copyfile(result, log_dir / "mismatch-current.csv")
                    shutil.copyfile(schema, log_dir / "mismatch-schema.csv")
                    raise
                with input_schema.open(newline="") as stream:
                    input_types = {r["column"]: r["kind"] for r in csv.DictReader(stream)}
                if case.startswith("string-") and input_types["k"] != "STR":
                    raise RuntimeError("String fixture did not produce STR keys")
                if case == "symbol-key" and input_types["k"] != "SYM":
                    raise RuntimeError("Symbol fixture did not produce SYM keys")
                if case in ("guid-key", "list-key") and input_types["k"] != case.split("-")[0].upper():
                    raise RuntimeError("Wide key fixture has wrong input type")
                if case in ("symbol-extrema", "symbol-top-bottom") and input_types["text"] != "SYM":
                    raise RuntimeError("Symbol fixture did not produce SYM values")
                if case.startswith("wide-") and input_types["text"] != "STR":
                    raise RuntimeError("Wide fixture did not produce STR values")
                records.append(dict(input_types=input_types, phases=profile_phases(run.stdout + run.stderr),
                                    case=case, workers=worker, binary=name, path=str(binary), round=round_number,
                                    rows=int(values[0]), groups=int(values[2]), cold_ms=values[1], warm_ms=values[3:8],
                                    peak_rss_kib=int(rss.read_text()), result_sha256=hashlib.sha256(result.read_bytes()).hexdigest()))
                args.output.write_text(json.dumps(records, indent=2) + "\n")
                print(f"{case} {name} workers={worker} round={round_number}: cold={values[1]:.3f}, warm={statistics.median(values[3:8]):.3f} ms", flush=True)
    print(f"Verified typed results for all {len(records)} runs.")


if __name__ == "__main__":
    main()
