#!/usr/bin/env python3
"""P5 float-store summary: per-column bytes and mix, store sizes, save
times, warm / first / cold medians g2 / g0, answer identity, and the
scan-only warm line (spec Benchmarks; pre-flight D9).  Also the carried
items: the ALP / ALP_RD exception rates and the DICT vectors with RAW codes
per column (O1, the short DICT tail), and the writer cost against BASE (O5).
Every number comes from the TSV; nothing is typed."""
import collections, statistics, sys
m = collections.defaultdict(list); c = collections.defaultdict(list); wall = collections.defaultdict(list)
cols, vecs, sizes, saves, ident, savet, meta, waits = [], [], {}, {}, {}, collections.defaultdict(list), {}, []
for line in open(sys.argv[1]):
    f = line.rstrip("\n").split("\t")
    if f[0] == "col": cols.append(f[1:])
    elif f[0] == "vec": vecs.append(f[1:])
    elif f[0] == "size": sizes[f[1]] = int(f[2])
    elif f[0] == "save": saves[f[1]] = float(f[2])
    elif f[0] == "meta": meta[f[1]] = f[2:]
    elif f[0] == "wait": waits.append(f[1:])
    elif f[0] == "ident": ident[f[1]] = f[2:]
    elif f[0] == "savet" and len(f) >= 5 and f[3] and f[4]:
        savet[(f[1], "g0")].append(float(f[3])); savet[(f[1], "g2")].append(float(f[4]))
        if len(f) >= 7: savet[(f[1], "size_g0")].append(int(f[5])); savet[(f[1], "size_g2")].append(int(f[6]))
    elif f[0] == "m":
        t = [float(x) for x in f[4:] if x]
        if len(t) == 6: m[(f[1], f[2], "first")].append(t[0]); m[(f[1], f[2], "warm")].append(statistics.median(t[1:]))
    elif f[0] == "wall": wall[(f[1], f[2])].append(float(f[4]))
    elif f[0] == "c" and len(f) > 4 and f[4]: c[(f[1], f[2])].append(float(f[4]))
print(f"meta {meta}")
print("| column | encoded B | raw B | ratio | mix |\n|---|---|---|---|---|")
for name, e, r, mix in cols: print(f"| {name} | {e} | {r} | {int(r) / max(int(e), 1):.2f}x | {mix} |")
if vecs:
    print("\n| column | type | vectors | ALP vecs | ALP exc / rows | ALP (e,f) | ALP_RD vecs | ALP_RD exc / rows | DICT vecs | DICT w/ RAW codes (last vec) | trailers |")
    print("|---|---|---|---|---|---|---|---|---|---|---|")
    for v in vecs:
        name, ty, rows, nv, a, ar, ae, d, dr, de, dc, dcr, dcl, tr, ef = v
        rate = lambda x, y: f"{int(x)} / {int(y)} ({100.0 * int(x) / int(y):.2f}%)" if int(y) else "-"
        print(f"| {name} | {ty} | {nv} | {a} | {rate(ae, ar)} | {ef} | {d} | {rate(de, dr)} | {dc} | {dcr} ({dcl}) | {tr} |")
print(f"\nstore: g0 {sizes.get('g0', 0)} B, g2 {sizes.get('g2', 0)} B (x{sizes.get('g2', 0) / max(sizes.get('g0', 1), 1):.3f}); "
      f"save g0 {saves.get('g0', 0):.0f} ms, g2 {saves.get('g2', 0):.0f} ms (prep, one run)\n")
if savet:
    print("| writer (O5) | save g0 ms | save g2 ms | g2 / g0 | runs |\n|---|---|---|---|---|")
    for b in ("p5", "base"):
        if savet[(b, "g0")]:
            s0 = statistics.median(savet[(b, "g0")]); s2 = statistics.median(savet[(b, "g2")])
            print(f"| {b} | {s0:.0f} | {s2:.0f} | {s2 / s0:.3f} | {len(savet[(b, 'g0')])} |")
    if savet[("p5", "g2")] and savet[("base", "g2")]:
        print(f"\nsave g2 p5 / base: {statistics.median(savet[('p5', 'g2')]) / statistics.median(savet[('base', 'g2')]):.3f}; "
              f"save g0 p5 / base: {statistics.median(savet[('p5', 'g0')]) / statistics.median(savet[('base', 'g0')]):.3f}")
    if savet[("base", "size_g2")]:
        print(f"g2 store bytes: p5 {savet[('p5', 'size_g2')][0]}, base {savet[('base', 'size_g2')][0]}; "
              f"g0: p5 {savet[('p5', 'size_g0')][0]}, base {savet[('base', 'size_g0')][0]}\n")
print("| query | warm g0 | warm g2 | ratio | first g0 | first g2 | cold g0 | cold g2 | ratio | answers |\n|---|---|---|---|---|---|---|---|---|---|")
qs = sorted({k[1] for k in m}, key=lambda q: int(q[1:])); worst = 0.0; same = True
for q in qs:
    w0 = statistics.median(m[("g0", q, "warm")]); w2 = statistics.median(m[("g2", q, "warm")])
    f0 = statistics.median(m[("g0", q, "first")]); f2 = statistics.median(m[("g2", q, "first")])
    c0 = statistics.median(c[("g0", q)]) if c[("g0", q)] else float("nan")
    c2 = statistics.median(c[("g2", q)]) if c[("g2", q)] else float("nan")
    i = ident.get(q)
    if i is None: ok, how = False, "missing"
    elif i[0] == i[1]: ok, how = True, "same"
    elif i[3] == i[4]: ok, how = True, "same sorted"
    elif i[0] != i[2] and i[3] != i[5]: ok, how = False, "DIFF (g0 drifts against itself too)"
    else: ok, how = False, "DIFF"
    same &= ok; worst = max(worst, w2 / w0)
    print(f"| {q} | {w0:.2f} | {w2:.2f} | {w2 / w0:.3f} | {f0:.2f} | {f2:.2f} | {c0:.1f} | {c2:.1f} | {c2 / c0:.3f} | {how} |")
if wall:
    print("\n| query | wall g0 ms/call | wall g2 ms/call | ratio |\n|---|---|---|---|")
    for q in sorted({k[1] for k in wall}, key=lambda q: int(q[1:])):
        a = statistics.median(wall[("g0", q)]); b = statistics.median(wall[("g2", q)])
        print(f"| {q} | {a:.2f} | {b:.2f} | {b / a if a > 0 else float('nan'):.3f} |")
if waits: print(f"\nload waits: {len(waits)} ({', '.join(w[0] + '/' + w[1] for w in waits[:10])})")
print(f"\n**P5 float store, answers g2 = g0: {'PASS' if same and qs else 'FAIL'}**")
print(f"**P5 float store, scan-only warm (no regression, <= 1.05 per query): {'PASS' if worst <= 1.05 and qs else 'FAIL'}** (worst {worst:.3f})")
