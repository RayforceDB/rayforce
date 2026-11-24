"""Median table of bench/affine_null/run.sh output: per shape and core count,
the median over rounds of each binary's per-round median, and the last
binary against the first.  Answers must agree across binaries and rounds."""
import collections, glob, os, statistics, sys

out, bins = sys.argv[1], sys.argv[2:]
ms, ans, order = collections.defaultdict(list), collections.defaultdict(set), []
for f in glob.glob(os.path.join(out, "b*.c*.r*.txt")):
    b, c = os.path.basename(f).split(".")[:2]
    for line in open(f):
        p = line.split(None, 2)
        if len(p) < 3:
            continue
        if p[0] not in order:
            order.append(p[0])
        ms[(p[0], int(b[1:]), int(c[1:]))].append(float(p[1]))
        ans[p[0]].add(p[2].strip())
cores = sorted({k[2] for k in ms})
names = " / ".join(os.path.basename(os.path.dirname(os.path.abspath(b))) or b for b in bins)
print("| shape | " + " | ".join(f"{c} core{'s' if c > 1 else ''}: {names}" for c in cores) + " |")
print("|---|" + "---:|" * len(cores))
bad = []
for s in order:
    cells = []
    for c in cores:
        xs = [statistics.median(ms[(s, i, c)]) for i in range(len(bins))]
        cell = " / ".join(f"{x:.2f}" for x in xs)
        if xs[0]:
            cell += f" ({100 * (xs[-1] / xs[0] - 1):+.0f}%)"
        cells.append(cell)
    print(f"| `{s}` | " + " | ".join(cells) + " |")
    if len(ans[s]) != 1:
        bad.append(s)
print()
print("answers differ: " + ", ".join(bad) if bad else "answers identical across binaries, cores and rounds")
