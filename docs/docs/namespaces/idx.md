# `.idx.*` — accelerator indexes

Attaches a secondary index to a column vector. The index is *metadata* — the underlying data is untouched (and shared via copy-on-write with any other holder of the same vector). The four index kinds cover the common query shapes:

- **`zone`** — min/max + null count per zone. Cheap range-predicate elimination on numeric and temporal columns.
- **`hash`** — open-addressing hash set of keys. O(1) point-lookup acceleration on `=` predicates.
- **`sort`** — sorted permutation. Enables binary search and order-aware merges.
- **`bloom`** — Bloom filter for set membership. Probabilistic, no false negatives, very compact.

Indexes are dropped automatically when the column is mutated (`alter set`, `update`, `insert`); a failed mutation does **not** drop the index since the data is unchanged. They survive being wrapped in a `.col.link` and a link survives an index drop — both metadata layers are independent.

## Reference

| Function | Arity | Flags | Description |
|---|---|---|---|
| [`.idx.zone`](#idx-zone) | unary | — | Attach a zone (min/max) index. |
| [`.idx.hash`](#idx-hash) | unary | — | Attach a hash-set index. |
| [`.idx.sort`](#idx-sort) | unary | — | Attach a sorted-permutation index. |
| [`.idx.bloom`](#idx-bloom) | unary | — | Attach a Bloom filter. |
| [`.idx.drop`](#idx-drop) | unary | — | Detach whatever index the vector carries. |
| [`.idx.has?`](#idx-has-p) | unary | — | True if the vector carries any index. |
| [`.idx.info`](#idx-info) | variadic | — | Dict describing the attached index, or a table column's format. |

## `.idx.zone` { #idx-zone }

Signature: `(.idx.zone v)`. Returns `v` with a zone index attached. `info` keys: `kind`, `length`, `parent_type`, `saved_attrs`, `min`, `max`, `n_nulls`. For float columns `min`/`max` are `f64`; for integer / temporal columns they are `i64`.

```lisp
(set tv  [5 1 9 3 7])
(set tvi (.idx.zone tv))
(.idx.has? tvi)   ;; => true
(.idx.info tvi)   ;; => {kind: 'zone, length: 5, min: 1, max: 9, n_nulls: 0, ...}
```

## `.idx.hash` { #idx-hash }

Signature: `(.idx.hash v)`. Returns `v` with a hash index attached. `info` keys add `capacity` (next power of two ≥ length) and `n_keys` (distinct keys hashed).

## `.idx.sort` { #idx-sort }

Signature: `(.idx.sort v)`. Returns `v` with a sorted permutation attached. `info` keys add `perm_len`.

## `.idx.bloom` { #idx-bloom }

Signature: `(.idx.bloom v)`. Returns `v` with a Bloom filter attached. `info` keys add `m_bits` (bit-array size, power of two), `k` (number of hash functions), `n_keys` (inserted distinct keys). False-positive rate is bounded by the standard Bloom formula for `m`, `k`, `n`.

## `.idx.drop` { #idx-drop }

Signature: `(.idx.drop v)`. Returns `v` with no index attached. Identity on an unindexed vector.

```lisp
(set t (.idx.drop tvi))
(.idx.has? t)   ;; => false
```

## `.idx.has?` { #idx-has-p }

Signature: `(.idx.has? v)`. Returns a bool. Safe to call on non-vectors (returns `false`).

```lisp
(.idx.has? tvi)   ;; => true
(.idx.has? tv)    ;; => false
(.idx.has? 42)    ;; => false
```

## `.idx.info` { #idx-info }

Signature: `(.idx.info v)`. Returns a dict if the vector carries an index, or the null object if not.

```lisp
(.idx.info (.idx.bloom [1 2 3 4 5]))
;; => {kind: 'bloom, length: 5, m_bits: 64, k: 4, n_keys: 5, ...}
```

Signature: `(.idx.info table 'col)`. Describes a table column's on-disk format without decoding it. Returns a dict:

| Key | Meaning |
|-----|---------|
| `gen` | `2` for a compressed column, `0` for a plain one (a ragged generation-0 parted table also reports `0`). |
| `enc` | Encoding name (`raw const for_bp delta_bp rle dict alp alp_rd str_dict fsst`) to vector count, nonzero counts only; empty for generation 0. |
| `encoded_bytes` | Bytes of the encoded column file (a parted column sums its segments). A synthetic stand-in segment (a ragged partition's absent column) adds nothing to `encoded_bytes` or the `enc` counts but does count in `raw_bytes`, and a plain parted segment counts its raw bytes. |
| `raw_bytes` | Bytes of the column as a plain vector (a string column includes its pool). |
| `materialized` | `true` once the plain column has been decoded and cached. |
| `index` | Generation 0: `.idx.info` of the attached index. Generation 2: the recorded index kind (`'none` when none); never materializes. |
| `segments` | Parted column only: the segment count. |

```lisp
(set T (.db.splayed.get "trades/"))
(.idx.info T 'price)
;; => {gen: 2, enc: {alp: 196}, encoded_bytes: ..., raw_bytes: ..., materialized: false, index: 'none}
```

## See also

- [Indexes Overview](../queries/indexes-overview.md) — when each kind helps and the query patterns it accelerates.
- [Accelerator Indexes](../queries/indexes.md) — query examples showing index-aware execution.
