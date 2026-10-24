#!/usr/bin/env python3
"""PI gate verdict from gate.tsv (written by pi_gate.sh).

Prints per-kind tables (warm / first run / 3-run total, head vs baseline, all
43 queries) and ONE gate line computed from the data:
  clause 1: no HASH index persisted by the head converter (every head store
            measured carries 0 HASH columns)
  clause 2: per load kind, warm head/baseline ratio <= 1.05 for every query
            (and result identity). A query is outside when the best-of-5 ratio > 1.05
            AND the gap is at least NOISE_MS AND the median-of-5-rounds ratio
            is also > 1.05 (a ~1 ms query's best-of-5 moves 15% on jitter
            alone); the others over 1.05 are listed as noise, never hidden.
q41/q42 on a splayed kind are the recorded deviation (scan speed vs the
baseline's persisted HASH); any other query outside is flagged UNEXPECTED.
"""
import statistics, sys
from collections import defaultdict

TOL = 1.05
NOISE_MS = 0.1
DEVIATION = {"q41", "q42"}
KINDS = ("gen0", "gen2", "parted")
NAME = {"gen0": "splayed-gen0", "gen2": "splayed-gen2", "parted": "parted"}

runs = defaultdict(list)          # (kind, variant, q) -> [[r1..r6], ...]
c1 = {}
ident = {}
meta = {}
# argv[1]: the full run; argv[2..]: confirmation runs (more rounds of a few
# queries); a confirmation run REPLACES the rows of the (kind, query) pairs it
# measured, it never adds to them.
files = sys.argv[1:]
replaced = set()
for fi, path in enumerate(files):
    if fi:
        for line in open(path):
            f = line.rstrip("\n").split("\t")
            if f[0] == "m":
                replaced.add((f[1], f[3]))
for fi, path in enumerate(files):
  for line in open(path):
    f = line.rstrip("\n").split("\t")
    if f[0] == "m":
        if fi == 0 and (f[1], f[3]) in replaced:
            continue
        runs[(f[1], f[2], f[3])].append([float(x) for x in f[5:11]])
    elif f[0] == "clause1":
        c1[f[1]] = int(f[3])
    elif f[0] == "ident":
        ident[(f[1], f[2])] = f[3:6]
    elif f[0] == "meta":
        meta[f[1]] = f[2]

wmed = {}

def stats(kind, v, q):
    rs = runs[(kind, v, q)]
    per = [statistics.median(r[1:]) for r in rs]
    warm = min(per)
    wmed[(kind, v, q)] = statistics.median(per)
    first = statistics.median(r[0] for r in rs)
    three = statistics.median(sum(r[:3]) for r in rs)
    return warm, first, three, len(rs)

def r(a, b):
    return a / b if b > 0 else 1.0

print("meta", meta)
print("clause1 HASH counts:", c1)
verdict = {}
firsts = {}
for kind in KINDS:
    qs = sorted({k[2] for k in runs if k[0] == kind}, key=lambda s: int(s[1:]))
    if not qs:
        continue
    print(f"\n## {NAME[kind]} (ms; head vs baseline; ratio = head/baseline)")
    print("| q | warm head | warm base | ratio | first head | first base | ratio | 3-run head | 3-run base | ratio |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    tot = [0.0] * 6
    outside, floor = [], []
    first_out = []
    for q in qs:
        wh, fh, th, n = stats(kind, "h", q)
        wd, fd, td, _ = stats(kind, "d", q)
        for i, x in enumerate((wh, wd, fh, fd, th, td)):
            tot[i] += x
        rw, rf, rt = r(wh, wd), r(fh, fd), r(th, td)
        mark = ""
        rwm = r(wmed[(kind, "h", q)], wmed[(kind, "d", q)])
        if rw > TOL:
            if wh - wd >= NOISE_MS and rwm > TOL:
                outside.append((q, rw)); mark = " **"
            else:
                floor.append((q, rw))
        if rf > TOL and fh - fd >= NOISE_MS:
            first_out.append((q, rf))
        print(f"| {q} | {wh:.3f} | {wd:.3f} | {rw:.3f}{mark} | {fh:.3f} | {fd:.3f} | {rf:.3f} | {th:.3f} | {td:.3f} | {rt:.3f} |")
    print(f"| total | {tot[0]:.1f} | {tot[1]:.1f} | {r(tot[0], tot[1]):.3f} | {tot[2]:.1f} | {tot[3]:.1f} | {r(tot[2], tot[3]):.3f} | {tot[4]:.1f} | {tot[5]:.1f} | {r(tot[4], tot[5]):.3f} |")
    print(f"warm outside {TOL}x (gap >= {NOISE_MS} ms): {[(q, round(x, 3)) for q, x in outside]}")
    print(f"warm over {TOL}x but not confirmed (gap < {NOISE_MS} ms or median-of-rounds ratio <= {TOL}: noise): {[(q, round(x, 3)) for q, x in floor]}")
    print(f"first run outside {TOL}x (gap >= {NOISE_MS} ms): {[(q, round(x, 3)) for q, x in first_out]}")
    real, tie = [], []
    for q in qs:
        i = ident.get((kind, q))
        if i and i[0] != i[1]:
            (tie if i[0] != i[2] else real).append(q)
    print(f"result text differs head vs baseline: stable-head diffs {real}; head unstable against itself (tie order) {tie}")
    verdict[kind] = (outside, real)
    firsts[kind] = first_out

ok1 = bool(c1) and all(n == 0 for k, n in c1.items() if k.startswith("head_"))
parts = []
for kind in KINDS:
    if kind not in verdict:
        continue
    outside, real = verdict[kind]
    exp = [(q, x) for q, x in outside if q in DEVIATION]
    unexp = [(q, x) for q, x in outside if q not in DEVIATION]
    if not outside and not real:
        parts.append(f"clause2 {NAME[kind]} PASS")
        continue
    why = ", ".join(f"{q} {x:.1f}x" for q, x in exp)
    if why:
        why += " warm"
    if real:
        why += (", " if why else "") + "results differ " + " ".join(real)
    if unexp:
        why += (", " if why else "") + "UNEXPECTED " + ", ".join(f"{q} {x:.2f}x" for q, x in unexp)
    tail = "; deviation recorded" if exp and not unexp and not real else ""
    parts.append(f"clause2 {NAME[kind]} FAIL ({why}{tail})")
fo = []
for kind in KINDS:
    if kind in firsts:
        fo.append(f"{NAME[kind]} " + (" ".join(q for q, _ in firsts[kind]) if len(firsts[kind]) < 10 else f"{len(firsts[kind])} queries") if firsts[kind] else f"{NAME[kind]} none")
print("\nPI GATE: clause1 " + ("PASS" if ok1 else "FAIL") + " ; " + " ; ".join(parts) + " ; first run over 1.05x (informational): " + "; ".join(fo))
