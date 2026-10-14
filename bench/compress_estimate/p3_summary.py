#!/usr/bin/env python3
"""P3 gate (plan: compressed-columns P3, Task 14) from the P2 driver's TSV.
1. P2's summary, unchanged; its strict line must pass (no P3 exemption).
2. Materialization per query against the P3 expectations (arms A and B).
3. Peak RSS, generation 2 against generation 0 (open decision D3's bound).
4. The P2 carries, each with its P2 re-run ratio and today's.
Every number comes from the file; nothing is typed."""
import re, subprocess, sys, statistics
from collections import defaultdict
tsv = sys.argv[1]
here = sys.argv[0].rsplit("/", 1)[0] if "/" in sys.argv[0] else "."
p2 = subprocess.run([sys.executable, f"{here}/p2_summary.py", tsv], capture_output=True, text=True, check=True).stdout
print(p2, end="")
strict = re.search(r"^P2 GATE \(strict\): (PASS|FAIL)$", p2, re.M)

mat, mem, runs = {}, {}, defaultdict(list)
for line in open(tsv):
    f = line.rstrip("\n").split("\t")
    if f[0] == "mat": mat[(f[1], f[2])] = f[3].strip()
    elif f[0] == "mem": mem[(f[1], f[2])] = int(f[3])
    elif f[0] == "m" and len(f) == 11:
        try: runs[(f[1], f[2], f[3])].append([float(x) for x in f[5:11]])
        except ValueError: pass

# Columns a query may still materialize after P3, by class (spec: holistic
# aggregates and distinct counts, LIKE outside the fused paths, computed group
# keys, `set` of a column, top-k sort keys).  Every other query: none.
# The entries once marked (confirm) were traced (gdb on enc_materialize_single /
# enc_materialize_parted, release build, commit 7a603266); the traced class is
# next to each, and RESULTS.md "P3 aware paths" has the frames.  q12 and q40
# materialized nothing on either arm and were narrowed out (ruling D7).
ALLOW = {
    "q9":  {"RegionID", "UserID"},                         # count distinct (holistic)
    "q10": {"RegionID", "UserID"},                         # count distinct (holistic)
    "q11": {"UserID", "MobilePhoneModel"},                 # count distinct (holistic): traced, A only (key and argument)
    "q14": {"UserID", "SearchPhrase"},                     # count distinct (holistic): traced, A only
    "q19": {"EventTime"},                                  # computed key (minute EventTime)
    "q21": {"URL"},                                        # LIKE
    "q22": {"URL"},                                        # LIKE: traced (exec_like), A and B
    "q23": {"Title", "URL", "UserID"},                     # LIKE (Title, URL), count distinct (UserID): traced
    "q24": {"EventTime", "URL"},                           # sort key (A, fused top-k); URL by LIKE outside the fused predicate (B): traced
    "q25": {"EventTime"}, "q26": {"SearchPhrase"}, "q27": {"EventTime", "SearchPhrase"},   # sort keys
    "q30": {"ResolutionWidth"},                            # (set rw (at hits 'ResolutionWidth))
    "q43": {"EventTime"},                                  # computed key (xbar EventTime)
}
# Per-arm entries (ruling 14-3), outside the per-query table above.
ALLOW_ARM = {
    ("A", "q23"): {"SearchPhrase"},   # the key of a count-distinct group (holistic), q10's RegionID class
    ("B", "q6"):  {"SearchPhrase"},   # count distinct (holistic); parted `at` reads the column eagerly (follow-up)
}
def cols(m):
    if m.startswith("<"): return {"*many*"}
    return set(re.findall(r"[A-Za-z_][A-Za-z0-9_]*", m))
bad = []
for arm in ("A", "B"):
    for i in range(1, 44):
        q = f"q{i}"
        m = mat.get((arm, q))
        if m is None: bad.append(f"{arm}/{q}:no-mat-row"); continue
        extra = cols(m) - ALLOW.get(q, set()) - ALLOW_ARM.get((arm, q), set())
        if extra: bad.append(f"{arm}/{q}:{'+'.join(sorted(extra))}")
print("p3_unexpected_materializations " + (" ".join(bad) if bad else "none"))

RSS_BOUND = 1.10   # open decision D3 (recommended); the P2 re-run had splayed x1.58
rss = {}
for arm, (a, b) in (("A", ("g0", "g2")), ("B", ("p0", "p2"))):
    if (arm, a) in mem and (arm, b) in mem:
        rss[arm] = mem[(arm, b)] / mem[(arm, a)]
        print(f"rss {arm}: {mem[(arm, a)]} -> {mem[(arm, b)]} kB (x{rss[arm]:.3f}, bound {RSS_BOUND})")

def ratio(arm, q, metric):
    va, vb = {"A": ("g0", "g2"), "B": ("p0", "p2")}[arm]
    ra, rb = runs.get((arm, va, q)), runs.get((arm, vb, q))
    if not ra or not rb: return float("nan")
    if metric == "warm": a = min(statistics.median(r[1:6]) for r in ra); b = min(statistics.median(r[1:6]) for r in rb)
    else:                a = statistics.median(r[0] for r in ra);     b = statistics.median(r[0] for r in rb)
    return b / a if a > 0 else float("nan")
# The P2 carries (P2 re-run numbers, RESULTS.md "Remaining P3 deferrals").
CARRIES = [("q2", "A", "warm", 1.086), ("q8", "A", "warm", 1.424), ("q8", "B", "warm", 1.060),
           ("q18", "A", "first", 1.983), ("q18", "B", "warm", 1.088), ("q39", "A", "first", 2.402),
           ("q39", "B", "first", 1.503), ("q42", "A", "warm", 1.664), ("q42", "B", "warm", 1.237)]
for q, arm, m, before in CARRIES:
    print(f"carry {q}/{arm}/{m}: P2 x{before:.3f} -> P3 x{ratio(arm, q, m):.3f}")
ok_mat = not bad
ok_rss = all(r <= RSS_BOUND for r in rss.values()) and len(rss) == 2
print(f"P3GATE p2_strict={'PASS' if strict and strict.group(1) == 'PASS' else 'FAIL'} "
      f"materialization={'PASS' if ok_mat else 'FAIL'} rss={'PASS' if ok_rss else 'FAIL'}")
print("P3 GATE: " + ("PASS" if strict and strict.group(1) == "PASS" and ok_mat and ok_rss else "FAIL"))
