#!/usr/bin/env python3
"""P5 Task 8, carried items O1 and the short float DICT tail: reads the
vector descriptors of every float column of a generation-2 splayed store
(spec "Format and codecs": directory at 96, 64 x 48-byte descriptors per
chunk, payload = null bitmap when n_nulls > 0, codec body, trailer) and
prints one `vec` line per column:
  vec <col> <type> <rows> <n_vec> <alp vecs> <alp rows> <alp exceptions>
      <alp_rd vecs> <alp_rd rows> <alp_rd exceptions> <dict vecs>
      <dict vecs with RAW codes> <of those, the column's last vector>
      <trailer vecs> <alp (e,f) pairs, count each>
Reads headers only; nothing is decoded.  usage: p5_vecscan.py <store dir>"""
import collections, os, struct, sys

ENC = {0: "raw", 1: "const", 2: "for_bp", 3: "delta_bp", 4: "rle", 5: "dict", 6: "alp", 7: "alp_rd"}
TYPES = {3: "I16", 6: "F32", 7: "F64"}   # the ray_t type byte (offset 18); rows at 24


def scan(path):
    b = open(path, "rb").read()
    if len(b) < 96 or b[32:36] != b"RENC":
        return None
    rows = struct.unpack_from("<q", b, 24)[0]
    typ = struct.unpack_from("<b", b, 18)[0]
    # the chunk count is ceil(rows / 65536) (spec); the directory of
    # n_chunks 64-byte entries must end where chunk 0 begins
    n_chunks = (rows + 65535) // 65536 if rows > 0 else 0
    if n_chunks:
        off0 = struct.unpack_from("<Q", b, 96)[0]
        if off0 < 96 + 64 * n_chunks or len(b) < 96 + 64 * n_chunks:
            sys.exit(f"{path}: directory of {n_chunks} chunks (ceil({rows} / 65536)) "
                     f"does not fit before chunk 0 at {off0}")
    n_vec_total = (rows + 1023) // 1024 if rows > 0 else 0
    s = collections.Counter(); efs = collections.Counter()
    v_index = 0
    for c in range(n_chunks):
        coff = struct.unpack_from("<Q", b, 96 + c * 64)[0]
        for j in range(64):
            if v_index >= n_vec_total:
                break
            d = coff + j * 48
            enc, _w, n_nulls, flags = struct.unpack_from("<BBHH", b, d)
            poff, pbytes = struct.unpack_from("<QQ", b, d + 8)
            n = 1024 if v_index < n_vec_total - 1 else rows - 1024 * (n_vec_total - 1)
            p = coff + poff + (128 if n_nulls > 0 else 0)
            s["n_vec"] += 1
            if flags & 2:
                s["trailer"] += 1
            if enc == 6:
                e, f, n_exc = struct.unpack_from("<BBH", b, p)
                s["alp"] += 1; s["alp_rows"] += n; s["alp_exc"] += n_exc; efs[(e, f)] += 1
            elif enc == 7:
                n_exc = struct.unpack_from("<H", b, p + 2)[0]
                s["alprd"] += 1; s["alprd_rows"] += n; s["alprd_exc"] += n_exc
            elif enc == 5:
                s["dict"] += 1
                if b[p] == 0:                       # the codes child is RAW
                    s["dict_raw"] += 1
                    if v_index == n_vec_total - 1:
                        s["dict_raw_last"] += 1
            v_index += 1
    return rows, typ, s, efs


def main():
    root = sys.argv[1].rstrip("/")
    cur = os.path.join(root, ".current")
    if os.path.isfile(cur):
        root = os.path.join(root, open(cur).readline().strip())
    for name in sorted(os.listdir(root)):
        p = os.path.join(root, name)
        if name.startswith(".") or not os.path.isfile(p):
            continue
        r = scan(p)
        if r is None:
            continue
        rows, typ, s, efs = r
        ef = ",".join(f"({e},{f})x{k}" for (e, f), k in efs.most_common()) or "-"
        print("\t".join(str(x) for x in ["vec", name, TYPES.get(typ, typ), rows, s["n_vec"], s["alp"], s["alp_rows"], s["alp_exc"],
                                         s["alprd"], s["alprd_rows"], s["alprd_exc"], s["dict"], s["dict_raw"],
                                         s["dict_raw_last"], s["trailer"], ef]))


main()
