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
# Scan-only ClickBench queries (1-based), pinned from the plan's "Scan-only
# query" decision before any run, never from the measured materializations.
SCAN_ONLY = {"q1", "q2", "q3", "q4", "q7", "q8", "q16", "q17", "q18", "q33", "q34", "q35", "q39"}
# Controller ruling: q18 (first-n pre-pass unstage) and q39 (conjunction
# gather via sel_compact) still materialize; the gather output contract is P3.
P3_DEFERRED = {"q18", "q39"}
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

verdict = {}; orders = defaultdict(list); worst = {}
for arm, (va, vb, title) in ARMS.items():
    if not any(k[0] == arm for k in runs): continue
    print(f"## arm {arm}: {title} (ms; ratio = {vb}/{va})")
    print("q\twarm_a\twarm_b\twr\tfirst_a\tfirst_b\tfr\t3run_a\t3run_b\ttr\tcold_a\tcold_b\tcr\tmaterialized\tanswers")
    rows = {}; tot = [0.0] * 6; missing = []
    for q in QS:
        sa, sb = stats(arm, va, q), stats(arm, vb, q)
        if not sa or not sb: missing.append(q); continue
        ca = statistics.median(cold[(arm, va, q)]) if cold.get((arm, va, q)) else float("nan")
        cb = statistics.median(cold[(arm, vb, q)]) if cold.get((arm, vb, q)) else float("nan")
        ans, note = answer(arm, q)
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
    scan = [q for q in QS if q in SCAN_ONLY and q in rows]
    other = [q for q in QS if q not in SCAN_ONLY and q in rows]
    v = {}
    v["answers"] = not missing and all(rows[q][4] == "same" for q in rows)
    def top5(qs, i): return sorted(((round(rows[q][i], 3), q) for q in qs), reverse=True)[:5]
    if arm in ("A", "B"):
        w = [rows[q][0] for q in scan]; c = [rows[q][3] for q in scan]; fr = [rows[q][1] for q in scan]
        v["warm"] = len(scan) == len(SCAN_ONLY) and all(x <= 1.05 for x in w) and gmean(w) <= 1.00
        v["cold"] = all(not math.isnan(x) for x in c) and gmean(c) < 1.00 and all(x <= 1.10 for x in c)
        v["first"] = gmean(fr) <= 1.00 and all(x <= 1.10 for x in fr)
        v["materializing_warm"] = all(rows[q][0] <= 1.05 for q in other)
        bad = sorted(q for q in SCAN_ONLY if mat.get((arm, q), "?") not in ("[]", ""))
        v["scan_only_zero"] = not bad
        worst[arm] = {"warm": top5(scan, 0), "cold": top5(scan, 3), "first": top5(scan, 1),
                      "materializing_warm": top5(other, 0)}
        print(f"scan_only n={len(scan)} warm_gmean={gmean(w):.3f} warm_worst={max(w, default=float('nan')):.3f} "
              f"first_gmean={gmean(fr):.3f} first_worst={max(fr, default=float('nan')):.3f} "
              f"cold_gmean={gmean(c):.3f} cold_worst={max(c, default=float('nan')):.3f}")
        print("scan_only_materializing " + (" ".join(f"{q}{'(P3-deferred)' if q in P3_DEFERRED else ''}" for q in bad) if bad else "none"))
        for cl, lst in worst[arm].items(): print(f"worst5 {cl} {lst}")
        if arm == "A":
            ft = ratio(tot[1], tot[4])
            v["first_total"] = ft < PI_SPLAYED_FIRST_TOTAL
            print(f"first_run_total_ratio {ft:.3f} (PI after P1: {PI_SPLAYED_FIRST_TOTAL})")
        for q in ("q41", "q42"):
            if q in rows: print(f"pi_row {q} warm {rows[q][0]:.3f} first {rows[q][1]:.3f} 3run {rows[q][2]:.3f}")
        v["_scan_only_bad"] = bad
    else:
        w = [rows[q][0] for q in rows]
        v["warm"] = len(rows) == 43 and all(x <= 1.05 for x in w) and gmean(w) <= 1.00
        worst[arm] = {"warm": top5(list(rows), 0)}
        print(f"gen0 warm_gmean={gmean(w):.3f} warm_worst={max(w, default=float('nan')):.3f} "
              f"worst5 {worst[arm]['warm']}")
        if arm == "C1" and "q16" in rows:
            s0, s1 = stats(arm, "base", "q16"), stats(arm, "p2", "q16")
            v["q16_first"] = s1[1] < s0[1]
            print(f"q16 first run base {s0[1]:.1f} ms, p2 {s1[1]:.1f} ms (carry 2)")
    verdict[arm] = v
    print()

for arm, lines in encs.items():
    delta = [l for l in lines if "delta" in l]
    print(f"enc {arm}: columns with DELTA vectors: {' '.join(delta) if delta else 'none'}")
if bad_rows: print("unparsed timing rows (query error?): " + "; ".join(bad_rows))
print(f"sizes {size}  peak_rss_kb {mem}  evict_modes {','.join(sorted(modes))}  ragged_partition {ragged}  meta {meta}")

def fields(arm, exempt):
    v = dict(verdict.get(arm, {}))
    bad = v.pop("_scan_only_bad", None)
    if exempt and bad is not None: v["scan_only_zero"] = not [q for q in bad if q not in P3_DEFERRED]
    return v
def fmt(arm, exempt):
    v = fields(arm, exempt)
    return " ".join(f"{k}={'PASS' if ok else 'FAIL'}" for k, ok in v.items()) or "NOT-RUN"
for exempt, tag in ((False, "strict"), (True, "P3-deferred q18 q39 exempt")):
    print(f"GATE ({tag}) splayed[{fmt('A', exempt)}] parted[{fmt('B', exempt)}] gen0-splayed[{fmt('C0', exempt)}] "
          f"gen0-parted[{fmt('C1', exempt)}] gen0-ragged[{fmt('R', exempt)}]")
    ok = all(arm in verdict for arm in ARMS) and all(all(fields(a, exempt).values()) for a in ARMS if a in verdict)
    print(f"P2 GATE ({tag}): " + ("PASS" if ok else "FAIL"))
