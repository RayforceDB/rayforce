#!/usr/bin/env python3
"""P2 gate from p2.tsv (plan: compressed-columns P2, Task 17).  Per-arm tables
and the GATE lines; every number comes from the file, nothing is typed."""
import math, statistics, sys
from collections import defaultdict

runs = defaultdict(list); cold = defaultdict(list); encs = defaultdict(list)
mat, mem, size, ident, meta, modes, ragged = {}, {}, {}, {}, {}, set(), None
bad_rows = []
for line in open(sys.argv[1]):
    f = line.rstrip("\n").split("\t")
    k = f[0]
    if k in ("m", "c"):
        try:
            if k == "m": runs[(f[1], f[2], f[3])].append([float(x) for x in f[5:11]]); assert len(f) == 11
            else:        cold[(f[1], f[2], f[3])].append(float(f[5])); modes.add(f[6])
        except (ValueError, IndexError, AssertionError):
            bad_rows.append("\t".join(f[:5]))
    elif k == "mat":                mat[(f[1], f[2])] = f[3].strip()
    elif k == "enc":                encs[f[1]].append(f[2])
    elif k == "mem":                mem[(f[1], f[2])] = int(f[3])
    elif k == "size":               size[(f[1], f[2])] = int(f[3])
    elif k == "ident":              ident[(f[1], f[2])] = f[3:10]
    elif k == "meta":               meta[f[1]] = f[2:]
    elif k == "ragged":             ragged = f[1:]

QS = [f"q{i}" for i in range(1, 44)]
# Cross-check only: the plan's pre-run "scan-only query" list.  The class a
# query is gated in comes from the materialization the run measured (mat rows).
SCAN_ONLY = {"q1", "q2", "q3", "q4", "q7", "q8", "q16", "q17", "q18", "q33", "q34", "q35", "q39"}
# Noise floor (gate-triage.md G, RESULTS.md "Gate noise floor"): a clause passes
# at ratio <= bound, or when both medians are < 1 ms and the gap is <= 0.05 ms.
# Geometric means cover only queries whose base median is >= 1 ms.
FLOOR_MS, FLOOR_GAP_MS = 1.0, 0.05
# P3 deferral table: (query, arms, metric, max_ratio, reason).  metric is
# warm | first | cold | rss.  Data only: the controller trims rows as the fix
# tasks land.
# GATE (P3-exempt) skips exactly these (query, arm, metric) clauses.
P3_DEFERRALS = [
    # (query, arms, metric, max_ratio, reason).  One row per arm, so each arm
    # has its own cap.  max_ratio is about 1.5x the ratio measured on the gate
    # re-run (HEAD a09a54eb); above it the exemption lapses and the clause
    # fails, so a regression of a deferred query still shows.  For rss the
    # ratio is the arm's peak-RSS ratio (informational, not gated).  Rows whose
    # clause passed on the re-run were removed.
    ("q2",  "A",  "warm",  1.63, "run-level compute (count over RLE runs)"),
    ("q8",  "A",  "warm",  2.14, "run-level compute (group over RLE runs)"),
    ("q8",  "B",  "warm",  1.59, "run-level compute (group over RLE runs)"),
    ("q18", "A",  "first", 2.97, "row-list gather (first-n keys published, P3 path 2)"),
    ("q18", "B",  "warm",  1.63, "row-list gather (parted first-n compact build, P3)"),
    ("q39", "A",  "first", 3.60, "selection gather (P3 path 2)"),
    ("q39", "B",  "first", 2.25, "selection gather (P3 path 2)"),
    ("q39", "A",  "cold",  3.23, "selection gather reads whole columns cold (P3)"),
    ("q42", "A",  "warm",  2.50, "point gathers of the selected keys (P3)"),
    ("q42", "B",  "warm",  1.86, "point gathers of the selected keys (P3)"),
    ("q24", "A",  "rss",   2.4,  "top-k output gather RSS"),
    ("q25", "A",  "rss",   2.4,  "top-k output gather RSS"),
    ("q26", "A",  "rss",   2.4,  "top-k output gather RSS"),
    ("q27", "A",  "rss",   2.4,  "top-k output gather RSS"),
]
PI_SPLAYED_FIRST_TOTAL = 2.99    # RESULTS.md "PI index policy": gen-2 first-run total 7 740 vs 2 586 ms
ARMS = {"A": ("g0", "g2", "splayed g0 vs g2"), "B": ("p0", "p2", "parted p0 vs p2"),
        "C0": ("base", "p2", "splayed g0, base vs p2 binary"),
        "C1": ("base", "p2", "parted p0, base vs p2 binary"),
        "R": ("base", "p2", "parted ragged, base vs p2 binary")}

def gmean(xs): return math.exp(sum(math.log(x) for x in xs) / len(xs)) if xs else float("nan")
def stats(arm, v, q):
    rs = runs.get((arm, v, q))
    if not rs: return None
    warm = min(statistics.median(r[1:6]) for r in rs)       # best round of the in-process medians
    first = statistics.median(r[0] for r in rs)
    three = statistics.median(sum(r[0:3]) for r in rs)
    return warm, first, three
def ratio(a, b): return b / a if a > 0 else float("nan")

def answer(arm, q):
    """same | DIFF | noise (b differs from itself), and an order note."""
    if (arm, q) not in ident: return "?", ""
    ra, rb, rb2, sa, sb, sb2, ordk = ident[(arm, q)]
    if ordk == "ordered":
        if ra == rb: return "same", ""
        if sa == sb: return "same", "tie-order"                  # same rows, ties ordered differently
    else:
        if sa == sb: return "same", ("order" if ra != rb else "")
    return ("noise" if sb != sb2 else "DIFF"), ""

verdict_gate = {}; allraw = {}; verdict = {}; orders = defaultdict(list); worst = {}
for arm, (va, vb, title) in ARMS.items():
    if not any(k[0] == arm for k in runs): continue
    print(f"## arm {arm}: {title} (ms; ratio = {vb}/{va})")
    print("q\twarm_a\twarm_b\twr\tfirst_a\tfirst_b\tfr\t3run_a\t3run_b\ttr\tcold_a\tcold_b\tcr\tmaterialized\tanswers")
    rows = {}; raw = {}; tot = [0.0] * 6; missing = []
    for q in QS:
        sa, sb = stats(arm, va, q), stats(arm, vb, q)
        if not sa or not sb: missing.append(q); continue
        ca = statistics.median(cold[(arm, va, q)]) if cold.get((arm, va, q)) else float("nan")
        cb = statistics.median(cold[(arm, vb, q)]) if cold.get((arm, vb, q)) else float("nan")
        ans, note = answer(arm, q)
        raw[q] = {0: (sa[0], sb[0]), 1: (sa[1], sb[1]), 3: (ca, cb)}
        if note: orders[arm].append(f"{q}:{note}")
        rows[q] = (ratio(sa[0], sb[0]), ratio(sa[1], sb[1]), ratio(sa[2], sb[2]), ratio(ca, cb), ans)
        for i in range(3): tot[i] += sa[i]; tot[3 + i] += sb[i]
        m = mat.get((arm, q), "")
        print(f"{q}\t{sa[0]:.2f}\t{sb[0]:.2f}\t{rows[q][0]:.3f}\t{sa[1]:.2f}\t{sb[1]:.2f}\t{rows[q][1]:.3f}\t"
              f"{sa[2]:.2f}\t{sb[2]:.2f}\t{rows[q][2]:.3f}\t{ca:.2f}\t{cb:.2f}\t{rows[q][3]:.3f}\t{m}\t"
              f"{ans}{'/' + note if note else ''}")
    print(f"totals over all 43: warm {tot[0]:.1f} vs {tot[3]:.1f} ({ratio(tot[0], tot[3]):.3f}); "
          f"first run {tot[1]:.1f} vs {tot[4]:.1f} ({ratio(tot[1], tot[4]):.3f}); "
          f"3-run {tot[2]:.1f} vs {tot[5]:.1f} ({ratio(tot[2], tot[5]):.3f})")
    if missing: print("missing " + " ".join(missing))
    diffs = [q for q in rows if rows[q][4] != "same"]
    print("answer_mismatches " + (" ".join(f"{q}:{rows[q][4]}" for q in diffs) if diffs else "none"))
    print("order_differences " + (" ".join(orders[arm]) if orders[arm] else "none"))
    def measured_scan(q): return mat.get((arm, q), "?") in ("[]", "")
    scan = [q for q in QS if q in rows and measured_scan(q)]
    other = [q for q in QS if q in rows and not measured_scan(q)]
    disagree = [q for q in QS if q in rows and (q in SCAN_ONLY) != measured_scan(q)]
    v = {}
    v["answers"] = not missing and all(rows[q][4] == "same" for q in rows)
    def top5(qs, i): return sorted(((round(rows[q][i], 3), q) for q in qs), reverse=True)[:5]
    def clauses(qs, i, bound, metric):
        """(query, passes) per query for ratio column i; a query below the
        exempt table's metric is marked, the caller decides to skip it."""
        out = []
        for q in qs:
            a_, b_ = raw[q][i]
            ok = rows[q][i] <= bound or (a_ < FLOOR_MS and b_ < FLOOR_MS and abs(b_ - a_) <= FLOOR_GAP_MS)
            out.append((q, ok))
        return out
    def big(qs, i): return [rows[q][i] for q in qs if raw[q][i][0] >= FLOOR_MS]
    if arm in ("A", "B"):
        # gated as scan-only: measured scan-only, plus any SCAN_ONLY-list query
        # that measured as materializing (it keeps its first and cold clauses)
        gate = [q for q in QS if q in rows and (measured_scan(q) or q in SCAN_ONLY)]
        other = [q for q in other if q not in gate]
        unexpected = [q for q in gate if not measured_scan(q) and q in SCAN_ONLY and mat.get((arm, q)) is not None]
        nomat = [q for q in rows if (arm, q) not in mat]
        v["_unexpected"] = unexpected; v["_matmissing"] = nomat
        v["_gate"] = gate
        v["_g"] = {m: [(q, rows[q][i], raw[q][i][0]) for q in gate if not math.isnan(rows[q][i])] for m, i in (("warm", 0), ("first", 1), ("cold", 3))}
        fails = {"warm": clauses(gate, 0, 1.05, "warm"), "first": clauses(gate, 1, 1.10, "first"),
                 "cold": [(q, ok) for q, ok in clauses(gate, 3, 1.10, "cold") if not math.isnan(rows[q][3])],
                 "materializing_warm": clauses(other, 0, 1.05, "warm")}
        v["_clauses"] = fails
        v["_cold_nan"] = any(math.isnan(rows[q][3]) for q in gate)
        w, fr, c = big(gate, 0), big(gate, 1), big(gate, 3)
        worst[arm] = {"warm": top5(gate, 0), "cold": top5(gate, 3), "first": top5(gate, 1),
                      "materializing_warm": top5(other, 0)}
        print(f"scan_only(gated) n={len(gate)}; gmeans over base>=1ms (per column): warm n={len(w)} {gmean(w):.3f}, "
              f"first n={len(fr)} {gmean(fr):.3f}, cold n={len(c)} {gmean(c):.3f}; worst warm={max([rows[q][0] for q in gate], default=float('nan')):.3f}")
        for m, i in (("warm", 0), ("first", 1), ("cold", 3)):
            sm = [q for q in gate if raw[q][i][0] < FLOOR_MS or math.isnan(raw[q][i][0])]
            print(f"below_1ms_excluded_from_{m}_gmean " + (" ".join(sm) if sm else "none"))
        print("mat rows missing " + (" ".join(nomat) if nomat else "none"))
        print("scan_only_list_but_materializing " + (" ".join(unexpected) if unexpected else "none"))
        print("measured_materializing " + (" ".join(f"{q}({len(mat.get((arm, q), '').strip('[]').split())} cols)" for q in QS if q in rows and not measured_scan(q)) if any(not measured_scan(q) for q in rows) else "none"))
        print("class_vs_SCAN_ONLY_list_disagreements " + (" ".join(f"{q}(measured {'scan' if measured_scan(q) else 'materializing'}, list {'scan' if q in SCAN_ONLY else 'other'})" for q in disagree) if disagree else "none"))
        for cl, lst in worst[arm].items(): print(f"worst5 {cl} {lst}")
        if arm == "A":
            ft = ratio(tot[1], tot[4])
            v["first_total"] = ft < PI_SPLAYED_FIRST_TOTAL
            print(f"first_run_total_ratio {ft:.3f} (PI after P1: {PI_SPLAYED_FIRST_TOTAL})")
        for q in ("q41", "q42"):
            if q in rows: print(f"pi_row {q} warm {rows[q][0]:.3f} first {rows[q][1]:.3f} 3run {rows[q][2]:.3f}")
    else:
        w = big(list(rows), 0)
        v["warm"] = len(rows) == 43 and all(ok for _, ok in clauses(list(rows), 0, 1.05, "warm")) and gmean(w) <= 1.00
        worst[arm] = {"warm": top5(list(rows), 0)}
        print(f"gen0 warm_gmean(base>=1ms n={len(w)})={gmean(w):.3f} warm_worst={max([rows[q][0] for q in rows], default=float('nan')):.3f} "
              f"worst5 {worst[arm]['warm']}")
        if arm == "C1" and "q16" in rows:
            s0, s1 = stats(arm, "base", "q16"), stats(arm, "p2", "q16")
            v["q16_first"] = s1[1] < s0[1]
            print(f"q16 first run base {s0[1]:.1f} ms, p2 {s1[1]:.1f} ms (carry 2)")
    allraw[arm] = (rows, raw); verdict_gate[arm] = set(v.get('_gate', ()))
    verdict[arm] = v
    print()

for arm, lines in encs.items():
    delta = [l for l in lines if "delta" in l]
    print(f"enc {arm}: columns with DELTA vectors: {' '.join(delta) if delta else 'none'}")
if bad_rows: print("unparsed timing rows (query error?): " + "; ".join(bad_rows))
print(f"sizes {size}  peak_rss_kb {mem}  evict_modes {','.join(sorted(modes))}  ragged_partition {ragged}  meta {meta}")

COL = {"warm": 0, "first": 1, "cold": 3}
def exempt_ok(arm, q, m):
    """The deferral for (q, arm, m) applies while its ratio stays <= max_ratio."""
    for dq, arms, dm, mx, _ in P3_DEFERRALS:
        if dq == q and dm == m and arm in arms:
            r = allraw[arm][0][q][COL[m]] if q in allraw[arm][0] else float("nan")
            return r <= mx
    return False
def deferred_any(arm, q):
    return any(dq == q and arm in arms and dm != "rss" for dq, arms, dm, _, _ in P3_DEFERRALS)
def fields(arm, exempt):
    v = dict(verdict.get(arm, {}))
    cl = v.pop("_clauses", None); g = v.pop("_g", None); cn = v.pop("_cold_nan", False)
    un = v.pop("_unexpected", []); nm = v.pop("_matmissing", []); v.pop("_gate", None)
    if cl is None: return v
    ex = (lambda q, m: exempt_ok(arm, q, m)) if exempt else (lambda q, m: False)
    out = {"answers": v["answers"]}
    for m in ("warm", "cold", "first"):
        gm = gmean([r for q, r, base in g[m] if base >= FLOOR_MS and not ex(q, m)])
        out[m] = (all(ok for q, ok in cl[m] if not ex(q, m)) and gm <= (1.00 if m != "cold" else 0.9999)
                  and not (m == "cold" and cn))
    out["materializing_warm"] = all(ok for q, ok in cl["materializing_warm"] if not ex(q, "warm"))
    out["no_unexpected_materialization"] = not nm and all(exempt and deferred_any(arm, q) for q in un)
    if "first_total" in v: out["first_total"] = v["first_total"]
    return out
def fmt(arm, exempt):
    v = fields(arm, exempt)
    return " ".join(f"{k}={'PASS' if ok else 'FAIL'}" for k, ok in v.items()) or "NOT-RUN"
for exempt, tag in ((False, "strict"), (True, "P3-exempt")):
    print(f"GATE ({tag}) splayed[{fmt('A', exempt)}] parted[{fmt('B', exempt)}] gen0-splayed[{fmt('C0', exempt)}] "
          f"gen0-parted[{fmt('C1', exempt)}] gen0-ragged[{fmt('R', exempt)}]")
    ok = all(arm in verdict for arm in ARMS) and all(all(fields(a, exempt).values()) for a in ARMS if a in verdict)
    print(f"P2 GATE ({tag}): " + ("PASS" if ok else "FAIL"))
parts = []
for q, arms, m, mx, why in P3_DEFERRALS:
    for arm in arms:
        if arm not in allraw: continue
        rows, raw = allraw[arm]
        if m == "rss":
            va, vb = ARMS[arm][0], ARMS[arm][1]
            a_, b_ = mem.get((arm, va)), mem.get((arm, vb))
            r = ratio(a_, b_) if a_ and b_ else float("nan")
            parts.append(f"{q}/{arm}/rss[{why}]: arm peak-RSS {a_} -> {b_} kB (x{r:.2f}, max {mx}) not gated"
                         + (" EXCEEDS-MAX" if r > mx else ""))
        elif q in rows:
            i = COL[m]
            a_, b_ = raw[q][i]; r = rows[q][i]
            bound = 1.05 if m == "warm" else 1.10
            gated = q in verdict_gate.get(arm, ()) or m == "warm"
            if math.isnan(r): st = "no data (not gated)"
            elif not gated: st = "not gated (materializing)"
            elif r <= bound or (a_ < FLOOR_MS and b_ < FLOOR_MS and abs(b_ - a_) <= FLOOR_GAP_MS): st = "within-bound"
            elif r <= mx: st = "EXEMPTED"
            else: st = f"EXEMPTION-LAPSED (x{r:.3f} > max {mx})"
            parts.append(f"{q}/{arm}/{m}[{why}]: {a_:.2f} -> {b_:.2f} ms (x{r:.3f}) {st}")
print("P3-EXEMPT LIST " + "; ".join(parts))
