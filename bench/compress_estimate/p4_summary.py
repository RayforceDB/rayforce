#!/usr/bin/env python3
"""P4 gate summary from p4_bench.sh's TSV: the index queries, generation 2
against generation 0 on the same release binary.
- warm: per process, the median of runs 2..6 (run 1 is the warm-up, where
  generation 2 materializes the column and attaches its index); the table
  gives the median over the rounds and the best round (P2's statistic).
- first: a fresh process with the store's files evicted from the page cache,
  median of the trials; reported, not gated (spec "Benchmarks").
- answers: every query's full answer, warm and first-touch, g0 against g2.
Gate: each warm median at most 1.05x generation 0, or, when both are below
1 ms, a gap of at most 0.05 ms (P2's noise floor, Ruling 14-8).  Every number
comes from the file; nothing is typed."""
import collections, statistics, sys
FLOOR_MS, FLOOR_GAP_MS, BOUND = 1.0, 0.05, 1.05
runs = collections.defaultdict(list)    # (v, q) -> [[t1..t6] per round]
firsts = collections.defaultdict(list)  # (v, q) -> [t per trial]
sizes, kinds, ident, uses, meta, waits, srckind = {}, {}, {}, {}, {}, [], ""
for line in open(sys.argv[1]):
    f = line.rstrip("\n").split("\t")
    k = f[0]
    if k == "size": sizes[f[1]] = int(f[2])
    elif k == "kind": kinds[f[1]] = f[2] if len(f) > 2 else ""
    elif k == "srckind": srckind = f[1] if len(f) > 1 else ""
    elif k == "meta": meta[f[1]] = f[2:]
    elif k == "loadwait": waits.append(f[1:])
    elif k == "m" and len(f) == 10:
        try: runs[(f[1], f[2])].append([float(x) for x in f[4:10]])
        except ValueError: pass
    elif k == "f" and len(f) == 5:
        try: firsts[(f[1], f[2])].append(float(f[4]))
        except ValueError: pass
    elif k == "ident": ident[(f[1], f[2])] = f[3:]
    elif k == "uses": uses[(f[1], f[2])] = "\t".join(f[3:])

QS = [f"q{i}" for i in range(1, 6)]
NAMES = {"q1": "UserID point lookup (ClickBench q20)", "q2": "RefererHash point lookup (q41's constant)",
         "q3": "URLHash point lookup (q42's constant)", "q4": "ClickBench q41", "q5": "ClickBench q42"}
def med(xs): return statistics.median(xs) if xs else float("nan")
def answers(mode, q):
    r = ident.get((mode, q))
    if not r: return "?"
    a, b, b2, sa, sb, sb2, order = r[:7]
    if b != b2 or sb != sb2: return "noise"
    if a == b: return "same"
    if sa == sb: return "order" if order == "ordered" else "same(sorted)"
    return "DIFF"
ok_warm, ok_ans, missing = True, True, []
print("| query | warm g0 ms | warm g2 ms | ratio | best round g0 / g2 (ratio) | in-process run 1 g0 / g2 | first touch g0 / g2 ms | rows | answers warm / first |")
print("|---|---|---|---|---|---|---|---|---|")
for q in QS:
    r0, r2 = runs.get(("g0", q)), runs.get(("g2", q))
    if not r0 or not r2: missing.append(q); ok_warm = False; continue
    m0 = [med(r[1:6]) for r in r0]; m2 = [med(r[1:6]) for r in r2]
    w0, w2 = med(m0), med(m2); b0, b2 = min(m0), min(m2)
    ratio = w2 / w0 if w0 > 0 else float("nan")
    ok = ratio <= BOUND or (w0 < FLOOR_MS and w2 < FLOOR_MS and abs(w2 - w0) <= FLOOR_GAP_MS)
    ok_warm &= ok
    p0, p2 = med([r[0] for r in r0]), med([r[0] for r in r2])
    f0, f2 = med(firsts.get(("g0", q), [])), med(firsts.get(("g2", q), []))
    aw, af = answers("warm", q), answers("first", q)
    ok_ans &= aw in ("same", "same(sorted)") and af in ("same", "same(sorted)")
    nrows = ident.get(("warm", q), [""] * 8)[7] if len(ident.get(("warm", q), [])) > 7 else "?"
    print(f"| {q} {NAMES[q]} | {w0:.3f} | {w2:.3f} | {ratio:.3f}{'' if ok else ' FAIL'} | {b0:.3f} / {b2:.3f} ({b2 / b0 if b0 > 0 else float('nan'):.3f}) "
          f"| {p0:.3f} / {p2:.3f} | {f0:.1f} / {f2:.1f} | {nrows} | {aw} / {af} |")
print(f"\nrounds: {min((len(v) for v in runs.values()), default=0)}; first-touch trials: {min((len(v) for v in firsts.values()), default=0)}")
print(f"sizes: g0 {sizes.get('g0', 0)} B, g2 {sizes.get('g2', 0)} B (x{sizes.get('g2', 0) / sizes['g0'] if sizes.get('g0') else float('nan'):.3f})")
print(f"index kinds (UserID RefererHash URLHash): source {srckind}; g0 {kinds.get('g0', '?')}; g2 {kinds.get('g2', '?')}")
for q in QS:
    print(f"uses {q}: g0 {uses.get(('g0', q), '?')} | g2 {uses.get(('g2', q), '?')}")
print("meta " + " ".join(f"{k}={'/'.join(v)}" for k, v in meta.items()))
print("load waits " + (" ".join("/".join(w) for w in waits) if waits else "none"))
if missing: print("missing " + " ".join(missing))
print(f"\n**P4 index answers: {'PASS' if ok_ans and not missing else 'FAIL'}** (every query's full answer, warm and first touch, g2 equal to g0)")
print(f"**P4 index queries warm: {'PASS' if ok_warm else 'FAIL'}** (each warm median at most {BOUND}x generation 0, or under {FLOOR_MS:.0f} ms within {FLOOR_GAP_MS} ms)")
