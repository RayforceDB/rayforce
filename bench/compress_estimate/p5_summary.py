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
nq = 10; bad_rows = []
# A per-query ratio is taken from the `wall` stage when the timeit g0 median
# is below FLOOR ms ((timeit q) reports a top-level reduction on a plain
# vector as ~0 ms); a wall time below FLOOR is below the stage's resolution
# and is floored to it, so two sub-resolution times compare as 1.0.
FLOOR = 0.05
XCHECK = 0.10       # the largest timeit / wall ratio gap printed without CHECK
def nums(xs):
    try: return [float(x) for x in xs if x]
    except ValueError: return None
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
    elif f[0] == "nq": nq = int(f[1])
    elif f[0] == "m":
        t = nums(f[4:])
        if t is not None and len(t) == 6: m[(f[1], f[2], "first")].append(t[0]); m[(f[1], f[2], "warm")].append(statistics.median(t[1:]))
        else: bad_rows.append(("warm", f[1], f[2], f[3]))
    elif f[0] == "wall":
        t = nums(f[4:5])
        if t: wall[(f[1], f[2])].append(t[0])
        else: bad_rows.append(("wall", f[1], f[2], f[3]))
    elif f[0] == "c":
        t = nums(f[4:5])
        if t: c[(f[1], f[2])].append(t[0])
        else: bad_rows.append(("cold", f[1], f[2], f[3]))
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
want = [f"q{i}" for i in range(nq)]
missing = []
for q in want:
    for what, have in (("warm", m.get(("g0", q, "warm")) and m.get(("g2", q, "warm"))),
                       ("cold", c.get(("g0", q)) and c.get(("g2", q))),
                       ("wall", wall.get(("g0", q)) and wall.get(("g2", q))),
                       ("ident", q in ident)):
        if not have: missing.append(f"{q}/{what}")
med = lambda xs: statistics.median(xs) if xs else float("nan")
ratio = lambda a, b: b / a if a > 0 else float("nan")
same = True; scan = {}
for q in want:
    w0 = med(m.get(("g0", q, "warm"))); w2 = med(m.get(("g2", q, "warm")))
    f0 = med(m.get(("g0", q, "first"))); f2 = med(m.get(("g2", q, "first")))
    c0 = med(c.get(("g0", q))); c2 = med(c.get(("g2", q)))
    i = ident.get(q)
    if i is None: ok, how = False, "missing"
    elif "FAIL" in i: ok, how = False, "FAIL (no answer)"
    elif i[0] == i[1]: ok, how = True, "same"
    elif i[3] == i[4]: ok, how = True, "same sorted"
    elif i[0] != i[2] and i[3] != i[5]: ok, how = False, "DIFF (g0 drifts against itself too)"
    else: ok, how = False, "DIFF"
    same &= ok
    print(f"| {q} | {w0:.2f} | {w2:.2f} | {ratio(w0, w2):.3f} | {f0:.2f} | {f2:.2f} | {c0:.1f} | {c2:.1f} | {ratio(c0, c2):.3f} | {how} |")
    a = med(wall.get(("g0", q))); b = med(wall.get(("g2", q)))
    if not (w0 >= FLOOR) and wall.get(("g0", q)) and wall.get(("g2", q)):
        fl = a < FLOOR or b < FLOOR
        scan[q] = (ratio(max(a, FLOOR), max(b, FLOOR)), "wall" + (" (floored)" if fl else ""))
    elif w0 >= FLOOR:
        rt = ratio(w0, w2)
        if wall.get(("g0", q)) and wall.get(("g2", q)):
            # both measures exist: the verdict takes the larger, and a gap
            # over XCHECK is flagged (a timeit artifact on one store, #856,
            # must not hide a regression the wall stage sees)
            rw = ratio(max(a, FLOOR), max(b, FLOOR))
            big = max(rt, rw) if rt == rt and rw == rw else float("nan")
            scan[q] = (big, f"max(timeit {rt:.3f}, wall {rw:.3f})" + (" CHECK" if not abs(rt - rw) <= XCHECK else ""))
        else:
            scan[q] = (rt, "timeit")
    else:
        scan[q] = (float("nan"), "none")
if wall:
    print("\n| query | wall g0 ms/call | wall g2 ms/call | ratio | rounds |\n|---|---|---|---|---|")
    for q in want:
        a = med(wall.get(("g0", q))); b = med(wall.get(("g2", q)))
        r = ratio(max(a, FLOOR), max(b, FLOOR)) if a < FLOOR or b < FLOOR else ratio(a, b)
        print(f"| {q} | {a:.3f} | {b:.3f} | {r:.3f}{' (floored)' if a < FLOOR or b < FLOOR else ''} | {len(wall.get(('g2', q), []))} |")
print("\n| query | scan-only ratio g2 / g0 | source |\n|---|---|---|")
for q in want: print(f"| {q} | {scan[q][0]:.3f} | {scan[q][1]} |")
if waits: print(f"\nload waits: {len(waits)} ({', '.join(w[0] + '/' + w[1] for w in waits[:10])})")
if bad_rows: print(f"\nunparsed rows: {len(bad_rows)} ({', '.join('/'.join(r) for r in bad_rows[:10])})")
# a row that erred (FAIL) or does not parse is a failure of its query,
# even when other rounds of it succeeded
for st, v, q, rnd in bad_rows: missing.append(f"{q}/{st} ({v} round {rnd} failed)")
if missing: print(f"\nmissing or failed (query/stage): {', '.join(missing)}")
complete = not missing
worst = max((v[0] for v in scan.values()), default=float("nan"))
wq = max(scan, key=lambda q: scan[q][0] if scan[q][0] == scan[q][0] else float("inf")) if scan else "-"
scan_ok = complete and all(v[0] <= 1.05 for v in scan.values())   # a NaN ratio fails
print(f"\n**P5 float store, all {nq} queries present (and no run failed) in warm, cold, wall and ident: {'PASS' if complete else 'FAIL'}**")
print(f"**P5 float store, answers g2 = g0: {'PASS' if same and complete else 'FAIL'}**")
print(f"**P5 float store, scan-only warm (no regression, <= 1.05 per query): {'PASS' if scan_ok else 'FAIL'}** (worst {worst:.3f}, {wq}, {scan.get(wq, (0, '-'))[1]})")
