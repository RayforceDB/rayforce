#!/usr/bin/env python3
"""P6 STR-store summary (copied from p5_summary.py): per STR column round
trip, encoding and runtime DICT against generation 0 (p6_dictcheck); per
column sizes and mix; store sizes, save times and the writer cost against
BASE (O2); warm / first / cold medians g2 / g0, answer identity, the wall
stage and the scan-only warm line (spec "Benchmarks", P5's wall / timeit
rule) over all 43 queries; the columns each query leaves materialized
(Rulings 6-2 / 7-1); the STR group-by queries against BASE (O5); the parted
arm (R2).  Every number comes from the TSV; nothing is typed."""
import collections, statistics, sys
m = collections.defaultdict(list); c = collections.defaultdict(list); wall = collections.defaultdict(list)
cols, dicts, gate, sizes, saves, ident, meta, waits = [], [], None, {}, {}, {}, {}, []
savet = collections.defaultdict(list); colsize = {}; est = None; matr = {}; pmat = {}; pident = {}
o5 = collections.defaultdict(list); pm = collections.defaultdict(list); pcols = []; skips = []; peaks = []; disks = []
answers = {}
nq = 43; bad_rows = []; o5_bad = []; pm_bad = []
FLOOR = 0.05        # P5: below it the wall stage gives the ratio, floored to it
XCHECK = 0.10       # the largest timeit / wall ratio gap printed without CHECK
def nums(xs):
    try: return [float(x) for x in xs if x]
    except ValueError: return None
def six(dst, bad, f):
    t = nums(f[4:])
    if t is not None and len(t) == 6:
        dst[(f[1], f[2], "first")].append(t[0]); dst[(f[1], f[2], "warm")].append(statistics.median(t[1:]))
    else: bad.append((f[0], f[1], f[2], f[3]))
for line in open(sys.argv[1], encoding="utf-8", errors="replace"):
    f = line.rstrip("\n").split("\t")
    k = f[0]
    if k == "col": cols.append(f[1:])
    elif k == "dict": dicts.append(f[1:])
    elif k == "dictgate": gate = f[1]
    elif k == "size": sizes[f[1]] = int(f[2])
    elif k == "est": est = int(f[2])
    elif k == "colsize": colsize[(f[1], f[2])] = int(f[3])
    elif k == "save": saves[f[1]] = float(f[2]) if f[2] else float("nan")
    elif k == "meta": meta[f[1]] = f[2:]
    elif k == "wait": waits.append(f[1:])
    elif k == "ident": ident[f[1]] = f[2:]
    elif k == "pident": pident[f[1]] = f[2:]
    elif k == "answer": answers[(f[1], f[2], f[3])] = f[4] if len(f) > 4 else ""
    elif k == "mat": matr[f[1]] = f[2]
    elif k == "pmat": pmat[f[1]] = f[2]
    elif k == "pcol": pcols.append(f[1:])
    elif k == "skip": skips.append(f[1:])
    elif k == "peak": peaks.append(f[1:])
    elif k == "disk": disks.append(f[1:])
    elif k == "savet" and len(f) >= 7 and f[3] and f[4]:
        savet[(f[1], "g0")].append(float(f[3])); savet[(f[1], "g2")].append(float(f[4]))
        savet[(f[1], "size_g0")].append(int(f[5])); savet[(f[1], "size_g2")].append(int(f[6]))
    elif k == "nq": nq = int(f[1])
    elif k == "m": six(m, bad_rows, f)
    elif k in ("o5c", "o5d"):
        t = nums(f[4:])
        if t is not None and len(t) == 6: o5[(k, f[1], f[2])].append(statistics.median(t[1:]))
        else: o5_bad.append((k, f[1], f[2], f[3]))
    elif k == "pm": six(pm, pm_bad, f)
    elif k == "wall":
        t = nums(f[4:5])
        if t: wall[(f[1], f[2])].append(t[0])
        else: bad_rows.append(("wall", f[1], f[2], f[3]))
    elif k == "c":
        t = nums(f[4:5])
        if t: c[(f[1], f[2])].append(t[0])
        else: bad_rows.append(("cold", f[1], f[2], f[3]))
med = lambda xs: statistics.median(xs) if xs else float("nan")
ratio = lambda a, b: b / a if a > 0 else float("nan")
print(f"meta {meta}")
# --- the STR columns: round trip and the runtime DICT (p6_dictcheck) ---
strcols = [d[0] for d in dicts]
print("| column | all STR_DICT | gen-2 index | DICT equal (codes, first occurrences, n_distinct) | elements equal | vectors | STR_DICT vectors | n_distinct g0 | n_distinct g2 |")
print("|---|---|---|---|---|---|---|---|---|")
for d in dicts: print("| " + " | ".join(d + [""] * (9 - len(d))) + " |")
rt_ok = len(dicts) > 0 and all(len(d) > 4 and d[4] == "true" for d in dicts)
dict_ok = gate == "PASS"
# --- per column bytes and mix ---
print("\n| column | g0 file B | g2 encoded B | raw B (.idx.info) | g0 / g2 | mix |\n|---|---|---|---|---|---|")
for name, e, r, mix in cols:
    g0b = colsize.get(("g0", name), 0)
    print(f"| {name}{' (STR)' if name in strcols else ''} | {g0b} | {e} | {r} | {g0b / max(int(e), 1):.2f}x | {mix} |")
print(f"\nstore: g0 {sizes.get('g0', 0)} B (estimate {est}), g2 {sizes.get('g2', 0)} B (x{sizes.get('g2', 0) / max(sizes.get('g0', 1), 1):.3f}); "
      f"save g0 {saves.get('g0', 0):.0f} ms, g2 {saves.get('g2', 0):.0f} ms (prep, one run)")
if "bg2" in sizes: print(f"BASE's generation-2 save of the same table (o5, RAW / CONST STR): {sizes['bg2']} B, save {saves.get('bg2', 0):.0f} ms")
if peaks: print("disk: " + "; ".join(f"{p[0]}: scratch {int(p[1])} B, free {int(p[2])} B" for p in peaks))
print()
if savet:
    print("| writer (O2) | save g0 ms | save g2 ms | g2 / g0 | g0 B | g2 B | runs |\n|---|---|---|---|---|---|---|")
    for b in ("p6", "base"):
        if savet[(b, "g0")]:
            s0 = med(savet[(b, "g0")]); s2 = med(savet[(b, "g2")])
            print(f"| {b} | {s0:.0f} | {s2:.0f} | {s2 / s0:.3f} | {savet[(b, 'size_g0')][0]} | {savet[(b, 'size_g2')][0]} | {len(savet[(b, 'g0')])} |")
    if savet[("p6", "g2")] and savet[("base", "g2")]:
        print(f"\nsave g2 p6 / base: {med(savet[('p6', 'g2')]) / med(savet[('base', 'g2')]):.3f}; "
              f"save g0 p6 / base: {med(savet[('p6', 'g0')]) / med(savet[('base', 'g0')]):.3f}\n")
want = [f"q{i}" for i in range(1, nq + 1)]
def matcols(s):
    s = (s or "").strip()
    if s in ("", "[]"): return []
    return s.strip("[]").replace("'", "").split()
def mclass(q):
    s = matr.get(q)
    if s is None: return "?"
    if s == "FAIL": return "FAIL"
    mc = matcols(s)
    return "scan" if not mc else "mat: " + " ".join(mc)
print("| query | warm g0 | warm g2 | ratio | first g0 | first g2 | cold g0 | cold g2 | ratio | answers | g2 materializes |\n|---|---|---|---|---|---|---|---|---|---|---|")
missing = []
for q in want:
    for what, have in (("warm", m.get(("g0", q, "warm")) and m.get(("g2", q, "warm"))),
                       ("cold", c.get(("g0", q)) and c.get(("g2", q))),
                       ("wall", wall.get(("g0", q)) and wall.get(("g2", q))),
                       ("ident", q in ident)):
        if not have: missing.append(f"{q}/{what}")
def how_same(i):
    if i is None: return False, "missing"
    if "FAIL" in i[:3]:
        if i[0] == i[1] == i[2] == "FAIL": return False, "error in both generations"
        return False, "FAIL (no answer)"
    if i[0] == i[1]: return True, "same"
    if i[3] == i[4]: return True, "same sorted"
    if i[0] != i[2] and i[3] != i[5]: return False, "DIFF (g0 drifts against itself too)"
    return False, "DIFF"
same = True; scan = {}; both_err = []
for q in want:
    w0 = med(m.get(("g0", q, "warm"))); w2 = med(m.get(("g2", q, "warm")))
    f0 = med(m.get(("g0", q, "first"))); f2 = med(m.get(("g2", q, "first")))
    c0 = med(c.get(("g0", q))); c2 = med(c.get(("g2", q)))
    ok, how = how_same(ident.get(q))
    if how == "error in both generations":
        both_err.append(f"{q} ({answers.get(('ident', q, 'a'), '')[:120]})")
    same &= ok
    print(f"| {q} | {w0:.2f} | {w2:.2f} | {ratio(w0, w2):.3f} | {f0:.2f} | {f2:.2f} | {c0:.1f} | {c2:.1f} | {ratio(c0, c2):.3f} | {how} | {mclass(q)} |")
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
print("\n| query | scan-only ratio g2 / g0 | source | g2 materializes |\n|---|---|---|---|")
for q in want: print(f"| {q} | {scan[q][0]:.3f} | {scan[q][1]} | {mclass(q)} |")
# --- Rulings 6-2 / 7-1: which queries leave a STR column materialized ---
strmat = {q: [x for x in matcols(matr.get(q)) if x in strcols] for q in want if matr.get(q) not in (None, "FAIL")}
print("\nSTR columns left materialized on g2 (mat stage): " +
      (", ".join(f"{q} {'+'.join(v)}" for q, v in strmat.items() if v) or "none"))
# --- O5: STR group-by against BASE ---
if o5:
    print("\n| query | C: g0 base | C: g0 p6 | ratio | D: base on its gen-2 save | D: p6 on g2 | ratio | p6 g2 / p6 g0 (warm stage) |\n|---|---|---|---|---|---|---|---|")
    for q in sorted({k[2] for k in o5}, key=lambda q: int(q[1:])):
        cb = med(o5.get(("o5c", "base", q))); cp = med(o5.get(("o5c", "p6", q)))
        db = med(o5.get(("o5d", "base", q))); dp = med(o5.get(("o5d", "p6", q)))
        w0 = med(m.get(("g0", q, "warm"))); w2 = med(m.get(("g2", q, "warm")))
        print(f"| {q} | {cb:.2f} | {cp:.2f} | {ratio(cb, cp):.3f} | {db:.2f} | {dp:.2f} | {ratio(db, dp):.3f} | {ratio(w0, w2):.3f} |")
    if o5_bad: print(f"\no5 failed rows: {len(o5_bad)} ({', '.join('/'.join(r) for r in o5_bad[:12])})")
# --- R2: the parted arm ---
pskip = [s for s in skips if s[0] == "parted"]
if pskip: print(f"\nparted arm (R2): skipped ({'; '.join(' '.join(s[1:]) for s in pskip)})")
if pm or pident:
    print(f"\nparted arm (R2): p0 {sizes.get('p0', 0)} B, p2 {sizes.get('p2', 0)} B; STR mixes: " +
          "; ".join(f"{p[0]} {p[1]}" for p in pcols if p[0] in strcols))
    print("\n| query | warm p0 | warm p2 | ratio | first p0 | first p2 | answers | p2 materializes |\n|---|---|---|---|---|---|---|---|")
    psame = True; pmiss = []
    for q in want:
        w0 = med(pm.get(("p0", q, "warm"))); w2 = med(pm.get(("p2", q, "warm")))
        f0 = med(pm.get(("p0", q, "first"))); f2 = med(pm.get(("p2", q, "first")))
        ok, how = how_same(pident.get(q)); psame &= ok
        if not (pm.get(("p0", q, "warm")) and pm.get(("p2", q, "warm"))): pmiss.append(q)
        pmc = pmat.get(q); pmc = "?" if pmc is None else ("scan" if not matcols(pmc) else "mat: " + " ".join(matcols(pmc)))
        print(f"| {q} | {w0:.2f} | {w2:.2f} | {ratio(w0, w2):.3f} | {f0:.2f} | {f2:.2f} | {how} | {pmc} |")
    for r in pm_bad: pmiss.append(f"{r[2]} ({r[1]} round {r[3]} failed)")
    print(f"\nparted arm (R2), informational: all {nq} queries present: {'yes' if not pmiss else 'no: ' + ', '.join(pmiss[:15])}; "
          f"answers p2 = p0: {'yes' if psame else 'no'}")
if waits: print(f"\nload waits: {len(waits)} ({', '.join(w[0] + '/' + w[1] for w in waits[:10])})")
if bad_rows: print(f"\nunparsed rows: {len(bad_rows)} ({', '.join('/'.join(r) for r in bad_rows[:10])})")
for st, v, q, rnd in bad_rows: missing.append(f"{q}/{st} ({v} round {rnd} failed)")
if both_err: print(f"\nerroring identically in both generations (listed, never counted as passing): {'; '.join(both_err)}")
if missing: print(f"\nmissing or failed (query/stage): {', '.join(missing)}")
complete = not missing
worst = max((v[0] for v in scan.values()), default=float("nan"))
wq = max(scan, key=lambda q: scan[q][0] if scan[q][0] == scan[q][0] else float("inf")) if scan else "-"
over = [f"{q} {scan[q][0]:.3f}" for q in want if not scan[q][0] <= 1.05]
scan_ok = complete and not over   # a NaN ratio fails
if over: print(f"\nover 1.05 (scan-only line): {', '.join(over)}")
print(f"\n**P6 gate (STR round trip): {'PASS' if rt_ok else 'FAIL'}** (every STR column element-identical, {len(dicts)} columns)")
note = " (not exercised: no all-STR_DICT column)" if gate == "not exercised" else ""
print(f"**P6 gate (DICT index equal to generation 0 on all-STR_DICT columns): {'PASS' if dict_ok else 'FAIL'}**{note}")
print(f"**P6 STR store, all {nq} queries present (and no run failed) in warm, cold, wall and ident: {'PASS' if complete else 'FAIL'}**")
print(f"**P6 STR store, answers g2 = g0: {'PASS' if same and complete else 'FAIL'}**")
print(f"**P6 STR store, scan-only warm (no regression, <= 1.05 per query): {'PASS' if scan_ok else 'FAIL'}** (worst {worst:.3f}, {wq}, {scan.get(wq, (0, '-'))[1]})")
