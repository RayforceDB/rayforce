#!/usr/bin/env python3
"""Writes the tiny encoder the tests of .emb.* open (test/data/emb/tiny), and
prints the vectors it must give, computed here with nothing but the standard
library: the same arithmetic as the engine's, written apart from it.

    python3 scripts/emb-tiny-model.py test/data/emb/tiny
"""
import json, math, os, random, struct, sys

H, HEADS, INNER, POS, LAYERS = 8, 2, 16, 16, 2
VOCAB = [("<s>", 0.0), ("<pad>", 0.0), ("</s>", 0.0), ("<unk>", 0.0), ("▁", -4.0),
         ("▁hello", -1.0), ("▁world", -1.0), ("▁he", -3.0), ("llo", -3.0),
         ("a", -5.0), ("b", -5.0), ("c", -5.0), ("▁a", -2.0), ("▁cat", -1.5), (",", -2.5), ("!", -2.5)]

f32 = lambda x: struct.unpack("<f", struct.pack("<f", x))[0]

def weights():
    """name -> (shape, rows); a vector is one row."""
    rng = random.Random(20261124)
    t = {}
    def put(name, rows, cols, scale=0.5, base=0.0):
        t[name] = ([rows, cols] if rows else [cols],
                   [[f32(base + scale * rng.gauss(0.0, 1.0)) for _ in range(cols)] for _ in range(rows or 1)])
    put("embeddings.word_embeddings.weight", len(VOCAB), H)
    put("embeddings.position_embeddings.weight", POS, H)
    put("embeddings.token_type_embeddings.weight", 2, H)
    put("embeddings.LayerNorm.weight", 0, H, 0.1, 1.0)
    put("embeddings.LayerNorm.bias", 0, H, 0.1)
    for l in range(LAYERS):
        p = f"encoder.layer.{l}."
        for part in ("query", "key", "value"):
            put(p + f"attention.self.{part}.weight", H, H)
            put(p + f"attention.self.{part}.bias", 0, H, 0.1)
        put(p + "attention.output.dense.weight", H, H)
        put(p + "attention.output.dense.bias", 0, H, 0.1)
        put(p + "attention.output.LayerNorm.weight", 0, H, 0.1, 1.0)
        put(p + "attention.output.LayerNorm.bias", 0, H, 0.1)
        put(p + "intermediate.dense.weight", INNER, H)
        put(p + "intermediate.dense.bias", 0, INNER, 0.1)
        put(p + "output.dense.weight", H, INNER)
        put(p + "output.dense.bias", 0, H, 0.1)
        put(p + "output.LayerNorm.weight", 0, H, 0.1, 1.0)
        put(p + "output.LayerNorm.bias", 0, H, 0.1)
    return t

def forward(t, ids):
    W = lambda name: t[name][1]
    V = lambda name: t[name][1][0]
    def norm(x, name):
        w, b, out = V(name + ".weight"), V(name + ".bias"), []
        for r in x:
            m = sum(r) / len(r)
            v = sum((a - m) ** 2 for a in r) / len(r)
            out.append([(a - m) / math.sqrt(v + 1e-12) * w[j] + b[j] for j, a in enumerate(r)])
        return out
    def dense(x, name):   # a weight is [out][in]
        w, b = W(name + ".weight"), V(name + ".bias")
        return [[sum(r[i] * w[o][i] for i in range(len(r))) + b[o] for o in range(len(w))] for r in x]
    n, dh = len(ids), H // HEADS
    word, pos, typ = W("embeddings.word_embeddings.weight"), W("embeddings.position_embeddings.weight"), W("embeddings.token_type_embeddings.weight")[0]
    x = norm([[word[ids[i]][j] + pos[i][j] + typ[j] for j in range(H)] for i in range(n)], "embeddings.LayerNorm")
    for l in range(LAYERS):
        p = f"encoder.layer.{l}."
        q, k, v = (dense(x, p + "attention.self." + s) for s in ("query", "key", "value"))
        ctx = [[0.0] * H for _ in range(n)]
        for h in range(HEADS):
            cols = range(h * dh, (h + 1) * dh)
            for i in range(n):
                s = [sum(q[i][d] * k[j][d] for d in cols) / math.sqrt(dh) for j in range(n)]
                top = max(s)
                e = [math.exp(a - top) for a in s]
                z = sum(e)
                for d in cols:
                    ctx[i][d] = sum(e[j] / z * v[j][d] for j in range(n))
        a = dense(ctx, p + "attention.output.dense")
        x = norm([[x[i][j] + a[i][j] for j in range(H)] for i in range(n)], p + "attention.output.LayerNorm")
        mid = [[0.5 * a * (1.0 + math.erf(a / math.sqrt(2.0))) for a in r] for r in dense(x, p + "intermediate.dense")]
        o = dense(mid, p + "output.dense")
        x = norm([[x[i][j] + o[i][j] for j in range(H)] for i in range(n)], p + "output.LayerNorm")
    v = [sum(x[i][j] for i in range(n)) / n for j in range(H)]
    length = math.sqrt(sum(a * a for a in v))
    return [a / length for a in v]

def safetensors(path, t):
    head, blobs, at = {}, [], 0
    for name, (shape, rows) in t.items():
        blob = b"".join(struct.pack("<f", a) for r in rows for a in r)
        head[name] = {"dtype": "F32", "shape": shape, "data_offsets": [at, at + len(blob)]}
        at += len(blob)
        blobs.append(blob)
    text = json.dumps(head, separators=(",", ":")).encode()
    text += b" " * (-len(text) % 8)
    with open(path, "wb") as f:
        f.write(struct.pack("<Q", len(text)) + text + b"".join(blobs))

def main(out):
    os.makedirs(out, exist_ok=True)
    t = weights()
    safetensors(os.path.join(out, "model.safetensors"), t)
    with open(os.path.join(out, "config.json"), "w") as f:
        json.dump({"model_type": "bert", "hidden_size": H, "num_hidden_layers": LAYERS, "num_attention_heads": HEADS,
                   "intermediate_size": INNER, "max_position_embeddings": POS, "type_vocab_size": 2,
                   "vocab_size": len(VOCAB), "hidden_act": "gelu", "layer_norm_eps": 1e-12}, f, indent=1)
        f.write("\n")
    with open(os.path.join(out, "tokenizer.json"), "w") as f:
        json.dump({"version": "1.0", "normalizer": None,
                   "model": {"type": "Unigram", "unk_id": 3, "vocab": [[s, w] for s, w in VOCAB]}}, f, ensure_ascii=False, indent=1)
        f.write("\n")
    for what, ids in (("hello world", [0, 5, 6, 2]), ("a cat, hello!", [0, 12, 13, 14, 5, 15, 2]),
                      ("the same cut to four tokens", [0, 12, 13, 2]),
                      ("hello twenty times, cut to the sixteen positions", [0] + [5] * 14 + [2]),
                      ("hello and a character the vocabulary lacks", [0, 5, 4, 3, 2])):
        print(what, ids)
        print("  [" + " ".join(f"{x:.7f}" for x in forward(t, ids)) + "]")

if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "test/data/emb/tiny")
