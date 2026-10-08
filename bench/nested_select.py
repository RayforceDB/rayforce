#!/usr/bin/env python3
"""Issue #752: compare literal nesting, do-materialization and hand composition.

Build: make release
Run:   python3 bench/nested_select.py --binary ./rayforce --rows 5000000
The baseline uses `do` to keep the inner select materialized, without a runtime
optimizer flag. All three variants must serialize identically before timing.
"""
import argparse
import os
from pathlib import Path
import statistics
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", default="./rayforce")
    parser.add_argument("--rows", type=int, default=5_000_000)
    parser.add_argument("--runs", type=int, default=7)
    parser.add_argument("--cores", type=int, default=4)
    args = parser.parse_args()
    if args.rows < 1 or args.runs < 1:
        parser.error("rows and runs must be positive")
    lines = [f"(set I (til {args.rows}))"]
    cases = []
    for width in (4, 40):
        cols = ["a", "b", "k", "p"] + [f"c{i}" for i in range(width - 4)]
        vals = ["(% I 3)", "(% I 5)", "(% I 100)", "(as 'F64 I)"] + [f"(+ I {i})" for i in range(width - 4)]
        lines.append(f"(set T (table [{' '.join(cols)}] (list {' '.join(vals)})))")
        identity = " ".join(f"{c}:{c}" for c in cols)
        shapes = [
            ("from:T where:(== a 1)", "where:(== b 1) total:(sum p)",
             "from:T where:(and (== a 1) (== b 1)) total:(sum p)"),
            (f"from:T where:(== a 1) {identity} q:(* p 2)", "by:k total:(sum q)",
             "from:T where:(== a 1) by:k total:(sum (* p 2))"),
            ("from:T where:(== a 1)", "take:10", "from:T where:(== a 1) take:10"),
            (f"from:T {identity}", "where:(== a 1) total:(sum p)",
             "from:T where:(== a 1) total:(sum p)"),
        ]
        for number, (inner, outer, flat) in enumerate(shapes, 1):
            tag = f"{width}/S{number}"
            variants = {
                "materialized": f"(select {{from:(do (select {{{inner}}})) {outer}}})",
                "nested": f"(select {{from:(select {{{inner}}}) {outer}}})",
                "manual": f"(select {{{flat}}})",
            }
            for name, expr in variants.items():
                lines.append(f"(set B_{name} (ser {expr}))")
            for name in ("nested", "manual"):
                lines.append(f'(println "CHECK {tag} {name} %" (if (== (count B_materialized) (count B_{name})) (all (== B_materialized B_{name})) false))')
            for expr in variants.values():
                lines.append(f"(timeit {expr})")
            for run in range(args.runs):
                names = list(variants) if run % 2 == 0 else list(reversed(variants))
                for name in names:
                    lines.append(f'(println "TIME {tag} {name} %" (timeit {variants[name]}))')
            cases.append(tag)
    lines.append("(exit 0)")
    with tempfile.TemporaryDirectory(prefix="ray-nested-select-") as tmp:
        script = Path(tmp) / "bench.rfl"
        script.write_text("\n".join(lines) + "\n")
        result = subprocess.run([str(Path(args.binary).resolve()), str(script)],
                                env={**os.environ, "RAYFORCE_CORES": str(args.cores)},
                                capture_output=True, text=True, check=True)
    checks = [line for line in result.stdout.splitlines() if line.startswith("CHECK ")]
    if len(checks) != len(cases) * 2 or any(not line.endswith("true") for line in checks):
        raise RuntimeError(f"serialized results differ or benchmark failed:\n{result.stdout}\n{result.stderr}")
    times = {}
    for line in result.stdout.splitlines():
        if line.startswith("TIME "):
            _, case, variant, ms = line.split()
            times.setdefault((case, variant), []).append(float(ms))
    print(f"{args.rows:,} rows, {args.cores} workers, median of {args.runs} interleaved runs; all serialized results equal.")
    print("| Columns/shape | Materialized ms | Nested ms | Manual ms | Speedup |")
    print("|---|---:|---:|---:|---:|")
    for case in cases:
        values = [times[(case, name)] for name in ("materialized", "nested", "manual")]
        if any(len(v) != args.runs for v in values):
            raise RuntimeError("incomplete timing output")
        old, new, manual = map(statistics.median, values)
        print(f"| {case} | {old:.4f} | {new:.4f} | {manual:.4f} | {old / new:.2f}x |")


if __name__ == "__main__":
    main()
