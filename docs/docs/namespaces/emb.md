# `.emb.*` — embeddings and vector search

Distances between numeric vectors, exact nearest neighbours, the HNSW index for approximate ones, and the model that turns text into vectors in the first place. An embedding column is a list whose entries are numeric vectors of one length; see [Vector Search & HNSW](../graph/vector-search.md) for the worked reference, persistence and the `nearest:` clause of `select`.

These functions were global names (`cos-dist`, `knn`, `hnsw-build`, …) before they were gathered here. The old names are gone. The index has a sub-namespace of its own, `.emb.hnsw.*`, and is read from and written to disk with `get` and `set`, the path first, as tables are with `.db.splayed.get` and `.db.splayed.set`. `norm` is linear algebra and stays a global name, beside `mmu`, `inv` and the rest.

## Reference

| Function | Arity | Flags | Description |
|---|---|---|---|
| [`.emb.cos-dist`](#emb-cos-dist) | binary | — | Cosine distance between two vectors. |
| [`.emb.inner-prod`](#emb-inner-prod) | binary | — | Inner product of two vectors. |
| [`.emb.l2-dist`](#emb-l2-dist) | binary | — | Euclidean distance between two vectors. |
| [`.emb.knn`](#emb-knn) | variadic | — | Exact k nearest neighbours by brute force. |
| [`.emb.hnsw.build`](#emb-hnsw-build) | variadic | — | Build an HNSW index over an embedding column. |
| [`.emb.ann`](#emb-ann) | variadic | — | Approximate k nearest neighbours through an HNSW index. |
| [`.emb.hnsw.info`](#emb-hnsw-info) | unary | — | Dict describing an index. |
| [`.emb.hnsw.set`](#emb-hnsw-set) | binary | restricted | Persist an index to a directory. |
| [`.emb.hnsw.get`](#emb-hnsw-get) | unary | restricted | Load an index from a directory. |
| [`.emb.hnsw.free`](#emb-hnsw-free) | unary | restricted | Release an index early. |
| [`.emb.model.get`](#emb-model-get) | unary | restricted | Open a text-embedding model from a directory. |
| [`.emb.model.info`](#emb-model-info) | unary | — | Dict describing a model. |
| [`.emb.tokens`](#emb-tokens) | binary | — | The token ids a model cuts a text into. |
| [`.emb.encode`](#emb-encode) | variadic | — | The vector of a text, or of each of many. |

## `.emb.cos-dist` { #emb-cos-dist }

Signature: `(.emb.cos-dist a b)`. Cosine distance, `1 - cos(a, b)`: lower is closer. Both operands are numeric vectors of the same length.

```lisp
(.emb.cos-dist [1.0 0.0] [0.0 1.0])   ;; => 1.0
```

## `.emb.inner-prod` { #emb-inner-prod }

Signature: `(.emb.inner-prod a b)`. The inner product: higher is closer.

## `.emb.l2-dist` { #emb-l2-dist }

Signature: `(.emb.l2-dist a b)`. Euclidean distance: lower is closer.

## `.emb.knn` { #emb-knn }

Signature: `(.emb.knn col query k [metric])`. Scans every vector of `col` and returns a table `{_rowid, _dist}` of the `k` nearest to `query`, ascending by distance. `metric` is `'cosine` (default), `'l2` or `'ip`. No index is needed; it is the right tool for small columns and one-off queries.

```lisp
(.emb.knn vecs [0.1 0.2 0.3 0.4] 2)
```

## `.emb.hnsw.build` { #emb-hnsw-build }

Signature: `(.emb.hnsw.build col [metric] [M] [ef_construction])`. Returns an index handle. Defaults: `'cosine`, `M=16`, `ef_construction=200`.

## `.emb.ann` { #emb-ann }

Signature: `(.emb.ann handle query k [ef_search])`. Returns `{_rowid, _dist}` for the approximately `k` nearest, ascending by distance.

```lisp
(set h (.emb.hnsw.build vecs 'cosine 16 200))
(.emb.ann h [0.1 0.2 0.3 0.4] 2 50)
```

## `.emb.hnsw.info` { #emb-hnsw-info }

Signature: `(.emb.hnsw.info handle)`. A dict: `{nrows dim metric nlayers M efc}`.

## `.emb.hnsw.set` { #emb-hnsw-set }

Signature: `(.emb.hnsw.set path handle)`. Writes the index to a directory; the metric is kept.

## `.emb.hnsw.get` { #emb-hnsw-get }

Signature: `(.emb.hnsw.get path)`. Returns a handle to the index saved at `path`.

## `.emb.hnsw.free` { #emb-hnsw-free }

Signature: `(.emb.hnsw.free handle)`. Releases the index now. Optional: a handle is reference-counted and is released when the last binding to it goes.

## Text into vectors

A text-embedding model runs inside the engine: no service to call and no library to link. The model is a directory as its publisher ships it, holding `config.json`, `tokenizer.json` and `model.safetensors`; the engine reads the three with its own readers and computes on its worker pool with the kernels `mmu` and `gram` have. The weights are mapped, not loaded: every tensor is used where the file has it, nothing of them is copied, and a page is read from disk when a text first needs it, so opening a model costs the tokenizer alone and two processes that open one model share its memory.

```lisp
(set m (.emb.model.get "models/my-encoder"))
(set docs ["how to restart the service" "the quarterly numbers" "restarting after a crash"])
(set t (table [text vec] (list docs (.emb.encode m docs))))
(select {from: t nearest: (.emb.knn vec (.emb.encode m "restart")) take: 2})
```

What is read: encoders of the BERT family (`"model_type": "bert"` in `config.json`: absolute positions, GELU, layer norm after each sublayer) with a Unigram tokenizer and F32 weights. The vector of a text is the last layer averaged over its tokens and scaled to length one, so cosine distance and inner product order results alike. A model of another family is refused by name rather than computed wrongly.

Models differ in what they expect around a text; many are trained with a prefix such as `"query: "` or `"passage: "`, and that is the caller's to add. The text is tokenized exactly as given: leading and trailing whitespace is part of it, so `trim` a text first where the wrapper you compare against does.

## `.emb.model.get` { #emb-model-get }

Signature: `(.emb.model.get dir)`. Opens the model in `dir` and returns a handle. Opened the way other disk data is: with `get`, the path first. The handle is reference-counted and the model never changes, so binding it to another name shares the one model; it is released when the last binding goes, and there is no function to free it.

## `.emb.model.info` { #emb-model-info }

Signature: `(.emb.model.info handle)`. A dict: `{kind tokenizer vocab dim layers heads inner tokens}`: the family, how text is cut, the size of the vocabulary, the length of a vector, the encoder's shape, and the most tokens a text may have.

## `.emb.tokens` { #emb-tokens }

Signature: `(.emb.tokens handle text)`. The ids the model's tokenizer cuts `text` into, an I32 vector with the begin and end marks; a vector of strings gives a list of them. Use it to count what a text costs, or to see where a long one will be cut.

```lisp
(count (.emb.tokens m "how to restart the service"))
```

## `.emb.encode` { #emb-encode }

Signature: `(.emb.encode handle text [max-tokens])`. The vector of `text`: F32, of the model's `dim`, of length one. A vector of strings gives a list of vectors, one for each; that list is an embedding column as `.emb.knn` and `.emb.hnsw.build` take it.

Either way the worker pool is used. Many texts go to the workers whole, a text to each. One text, or fewer texts than there are workers, is computed by all the workers together: the rows of each matrix product and the heads of attention are shared out, so a single long text does not wait on one core.

A text of more tokens than the model allows (`tokens` in `.emb.model.info`), or than `max-tokens` when that is given and smaller, is cut to its beginning. The cost of a text grows faster than its length, so a smaller `max-tokens` is the way to trade the tail of long texts for speed.
