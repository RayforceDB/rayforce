# DAG & Execution C API

!!! note "Lazy evaluation"
    All `ray_*` DAG functions return `ray_op_t*` nodes. Nothing executes until you call `ray_execute(g, root)`. This allows the optimizer to rewrite the entire plan before any data flows.

## DAG Construction

### ray_graph_new

Creates a new operation graph bound to a source table. Column scans (`ray_scan`) resolve column names against this table. The table is retained.

```c
ray_graph_t* ray_graph_new(ray_t* tbl);
```

### ray_graph_free

Frees the operation graph and all its nodes. Does not release the bound table or results — the caller must release those separately.

```c
void ray_graph_free(ray_graph_t* g);
```

### Source Operations

Source operations produce data from tables, constants, or external vectors.

#### ray_scan

Scans a named column from the bound table. The name is interned into the symbol table. Returns an `OP_SCAN` node.

```c
ray_op_t* ray_scan(ray_graph_t* g, const char* col_name);
```

#### ray_const_i64 / ray_const_f64 / ray_const_bool / ray_const_str

Create constant scalar nodes. These broadcast to match vector lengths during execution.

```c
ray_op_t* ray_const_i64(ray_graph_t* g, int64_t val);
ray_op_t* ray_const_f64(ray_graph_t* g, double val);
ray_op_t* ray_const_bool(ray_graph_t* g, bool val);
ray_op_t* ray_const_str(ray_graph_t* g, const char* s, size_t len);
```

#### ray_const_vec / ray_const_table

Inject a pre-built vector or table into the DAG. Useful for multi-table joins.

```c
ray_op_t* ray_const_vec(ray_graph_t* g, ray_t* vec);
ray_op_t* ray_const_table(ray_graph_t* g, ray_t* table);
```

## Unary Operations

Element-wise unary operations. These are fuseable — the optimizer merges chains of unary/binary ops into single morsel passes.

| Function | Opcode | Description |
|---|---|---|
| `ray_neg(g, a)` | `OP_NEG` | Arithmetic negation |
| `ray_abs(g, a)` | `OP_ABS` | Absolute value |
| `ray_not(g, a)` | `OP_NOT` | Logical NOT |
| `ray_sqrt_op(g, a)` | `OP_SQRT` | Square root |
| `ray_log_op(g, a)` | `OP_LOG` | Natural logarithm |
| `ray_exp_op(g, a)` | `OP_EXP` | Exponential (e^x) |
| `ray_ceil_op(g, a)` | `OP_CEIL` | Ceiling |
| `ray_floor_op(g, a)` | `OP_FLOOR` | Floor |
| `ray_isnull(g, a)` | `OP_ISNULL` | Returns BOOL: true if null |
| `ray_upper(g, a)` | `OP_UPPER` | Uppercase string |
| `ray_lower(g, a)` | `OP_LOWER` | Lowercase string |
| `ray_strlen(g, a)` | `OP_STRLEN` | String byte length |
| `ray_trim_op(g, a)` | `OP_TRIM` | Strip whitespace |

### ray_cast

Type cast. Converts the input to `target_type` (e.g., `RAY_F64`, `RAY_I64`).

```c
ray_op_t* ray_cast(ray_graph_t* g, ray_op_t* a, int8_t target_type);

/* Cast I64 column to F64 */
ray_op_t* price_f = ray_cast(g, ray_scan(g, "price"), RAY_F64);
```

## Binary Operations

Element-wise binary operations. All are fuseable into morsel passes.

| Function | Opcode | Description |
|---|---|---|
| `ray_add(g, a, b)` | `OP_ADD` | Addition |
| `ray_sub(g, a, b)` | `OP_SUB` | Subtraction |
| `ray_mul(g, a, b)` | `OP_MUL` | Multiplication |
| `ray_div(g, a, b)` | `OP_DIV` | Division |
| `ray_mod(g, a, b)` | `OP_MOD` | Modulo |
| `ray_eq(g, a, b)` | `OP_EQ` | Equal |
| `ray_ne(g, a, b)` | `OP_NE` | Not equal |
| `ray_lt(g, a, b)` | `OP_LT` | Less than |
| `ray_le(g, a, b)` | `OP_LE` | Less than or equal |
| `ray_gt(g, a, b)` | `OP_GT` | Greater than |
| `ray_ge(g, a, b)` | `OP_GE` | Greater than or equal |
| `ray_and(g, a, b)` | `OP_AND` | Logical AND |
| `ray_or(g, a, b)` | `OP_OR` | Logical OR |
| `ray_like(g, a, b)` | `OP_LIKE` | SQL LIKE (case-sensitive) |
| `ray_ilike(g, a, b)` | `OP_ILIKE` | SQL LIKE (case-insensitive) |
| `ray_min2(g, a, b)` | `OP_MIN2` | Element-wise minimum |
| `ray_max2(g, a, b)` | `OP_MAX2` | Element-wise maximum |

### ray_if

Ternary conditional: for each element, returns `then_val` where `cond` is true, `else_val` otherwise.

```c
ray_op_t* ray_if(ray_graph_t* g, ray_op_t* cond,
               ray_op_t* then_val, ray_op_t* else_val);
```

### String binary/ternary ops

DAG nodes for string operations. `ray_concat` accepts an array of N inputs.

```c
ray_op_t* ray_substr(ray_graph_t* g, ray_op_t* str,
                    ray_op_t* start, ray_op_t* len);
ray_op_t* ray_replace(ray_graph_t* g, ray_op_t* str,
                     ray_op_t* from, ray_op_t* to);
ray_op_t* ray_concat(ray_graph_t* g, ray_op_t** args, int n);
```

## Aggregation Operations

Reduction operations that collapse a column to a single value (or per-group values when combined with `ray_group`).

| Function | Opcode | Description |
|---|---|---|
| `ray_sum(g, a)` | `OP_SUM` | Sum of values |
| `ray_prod(g, a)` | `OP_PROD` | Product of values |
| `ray_count(g, a)` | `OP_COUNT` | Count of non-null values |
| `ray_avg(g, a)` | `OP_AVG` | Average (mean) |
| `ray_min_op(g, a)` | `OP_MIN` | Minimum value |
| `ray_max_op(g, a)` | `OP_MAX` | Maximum value |
| `ray_first(g, a)` | `OP_FIRST` | First non-null value |
| `ray_last(g, a)` | `OP_LAST` | Last non-null value |
| `ray_count_distinct(g, a)` | `OP_COUNT_DISTINCT` | Count of distinct values |
| `ray_stddev(g, a)` | `OP_STDDEV` | Sample standard deviation |
| `ray_stddev_pop(g, a)` | `OP_STDDEV_POP` | Population standard deviation |
| `ray_var(g, a)` | `OP_VAR` | Sample variance |
| `ray_var_pop(g, a)` | `OP_VAR_POP` | Population variance |

## Time-Series Vector Operations

Unary vector operations that preserve row count. These are lazy DAG nodes in Rayfall and materialize through morsel-based kernels.

| Function | Opcode | Description |
|---|---|---|
| `ray_lag_op(g, a)` | `OP_LAG` | Previous row value; first row is null/sentinel |
| `ray_lead_op(g, a)` | `OP_LEAD` | Next row value; last row is null/sentinel |
| `ray_deltas_op(g, a)` | `OP_DELTAS` | Adjacent differences; first row is null |
| `ray_ratios_op(g, a)` | `OP_RATIOS` | Adjacent ratios as `F64`; first row is null |
| `ray_fills_op(g, a)` | `OP_FILLS` | Forward-fill nullable vectors |
| `ray_sums_op(g, a)` | `OP_SUMS` | Running sum |
| `ray_avgs_op(g, a)` | `OP_AVGS` | Running average over non-null values |
| `ray_mins_op(g, a)` | `OP_MINS` | Running minimum |
| `ray_maxs_op(g, a)` | `OP_MAXS` | Running maximum |
| `ray_prds_op(g, a)` | `OP_PRDS` | Running product |
| `ray_differ_op(g, a)` | `OP_DIFFER` | Boolean change flag versus previous row |
| `ray_msum_op(g, a, window)` | `OP_MSUM` | Moving sum over trailing `window` rows |
| `ray_mavg_op(g, a, window)` | `OP_MAVG` | Moving average over trailing `window` rows and non-null values |
| `ray_mmin_op(g, a, window)` | `OP_MMIN` | Moving minimum over trailing `window` rows |
| `ray_mmax_op(g, a, window)` | `OP_MMAX` | Moving maximum over trailing `window` rows |
| `ray_mcount_op(g, a, window)` | `OP_MCOUNT` | Moving non-null count over trailing `window` rows |
| `ray_mvar_op(g, a, window)` | `OP_MVAR` | Moving population variance over trailing `window` rows |
| `ray_mdev_op(g, a, window)` | `OP_MDEV` | Moving population standard deviation over trailing `window` rows |

Moving-window constructors require a positive `int64_t` window. Rayfall syntax uses the same order, for example `(msum 20 price)`. In query projections, constant windows lower into DAG nodes; non-constant windows use the normal evaluator path.

## Structural Operations

Pipeline breakers that reshape data: filtering, sorting, grouping, joining, and projecting.

### ray_filter

Lazily filters a column by a boolean predicate. The predicate must produce a BOOL vector. Rows where the predicate is false (or null) are excluded.

```c
ray_op_t* ray_filter(ray_graph_t* g, ray_op_t* input,
                    ray_op_t* predicate);
```

### ray_sort_op

Multi-column sort. Pass arrays of key nodes, sort directions (1=descending), and null ordering (1=nulls first). Uses parallel radix sort for numerics, merge sort for strings.

```c
ray_op_t* ray_sort_op(ray_graph_t* g, ray_op_t* table_node,
                    ray_op_t** keys, uint8_t* descs,
                    uint8_t* nulls_first, uint32_t n_cols);
```

### ray_group

Group-by with aggregation. Groups by `n_keys` key columns, applying `n_aggs` aggregate operations (specified as `OP_SUM`, `OP_COUNT`, etc.) to the corresponding input columns.

```c
ray_op_t* ray_group(ray_graph_t* g,
                   ray_op_t** keys, uint32_t n_keys,
                   uint16_t* agg_ops, ray_op_t** agg_ins,
                   uint32_t n_aggs);
```

### ray_distinct

Returns distinct rows based on the given key columns.

```c
ray_op_t* ray_distinct(ray_graph_t* g, ray_op_t** keys, uint32_t n_keys);
```

### ray_join

Hash join between two tables on matching key columns. Join types: 0=inner, 1=left outer, 2=full outer. Uses radix-partitioned hash join with adaptive radix bits (2..14) to fit L2 cache.

```c
ray_op_t* ray_join(ray_graph_t* g,
                  ray_op_t* left_table, ray_op_t** left_keys,
                  ray_op_t* right_table, ray_op_t** right_keys,
                  uint32_t n_keys, uint8_t join_type);
```

### ray_asof_join

As-of join for time-series alignment. Matches each left row to the most recent right row with a time key ≤ the left's time key, optionally partitioned by equality keys.

```c
ray_op_t* ray_asof_join(ray_graph_t* g,
                       ray_op_t* left_table, ray_op_t* right_table,
                       ray_op_t* time_key,
                       ray_op_t** eq_keys, uint32_t n_eq_keys,
                       uint8_t join_type);
```

### ray_window_op

Window functions with partition keys, order keys, frame specification, and multiple function kinds (ROW_NUMBER, RANK, DENSE_RANK, NTILE, SUM, AVG, LAG, LEAD, FIRST_VALUE, LAST_VALUE, NTH_VALUE).

```c
ray_op_t* ray_window_op(ray_graph_t* g, ray_op_t* table_node,
    ray_op_t** part_keys, uint32_t n_part,
    ray_op_t** order_keys, uint8_t* order_descs, uint32_t n_order,
    uint8_t* func_kinds, ray_op_t** func_inputs,
    int64_t* func_params, uint32_t n_funcs,
    uint8_t frame_type, uint8_t frame_start,
    uint8_t frame_end,
    int64_t frame_start_n, int64_t frame_end_n);
```

### ray_head / ray_tail / ray_select_op

`ray_head` returns the first N rows, `ray_tail` returns the last N rows. `ray_select_op` projects specific columns from a table node.

```c
ray_op_t* ray_head(ray_graph_t* g, ray_op_t* input, int64_t n);
ray_op_t* ray_tail(ray_graph_t* g, ray_op_t* input, int64_t n);
ray_op_t* ray_select_op(ray_graph_t* g, ray_op_t* input,
                        ray_op_t** cols, uint32_t n_cols);
```

## Graph Operations

Graph traversal operations work on CSR edge indices (`ray_rel_t`). See [CSR / Relationship API](#csr) below to build the index.

### ray_expand

1-hop neighbor expansion. For each source node, outputs all neighbors from the CSR index. Direction: 0=forward, 1=reverse, 2=both.

```c
ray_op_t* ray_expand(ray_graph_t* g, ray_op_t* src_nodes,
                    ray_rel_t* rel, uint8_t direction);
```

### ray_var_expand

Variable-length BFS traversal from start nodes through `min_depth` to `max_depth` hops. With `track_path=true`, outputs the full path for each reached node.

```c
ray_op_t* ray_var_expand(ray_graph_t* g, ray_op_t* start_nodes,
                        ray_rel_t* rel, uint8_t direction,
                        uint8_t min_depth, uint8_t max_depth,
                        bool track_path);
```

### ray_shortest_path

BFS shortest path between source and destination nodes, up to `max_depth` hops.

```c
ray_op_t* ray_shortest_path(ray_graph_t* g,
    ray_op_t* src, ray_op_t* dst,
    ray_rel_t* rel, uint8_t max_depth);
```

### ray_wco_join

Worst-case optimal join via Leapfrog Triejoin. Enumerates multi-way patterns (triangles, k-cliques) over multiple relationships without materializing intermediate cross-products.

```c
ray_op_t* ray_wco_join(ray_graph_t* g,
                      ray_rel_t** rels, uint8_t n_rels,
                      uint8_t n_vars);
```

### Additional Graph Algorithms

| Function | Algorithm |
|---|---|
| `ray_pagerank(g, rel, max_iter, damping)` | Iterative PageRank |
| `ray_connected_comp(g, rel)` | Connected components (label propagation) |
| `ray_dijkstra(g, src, dst, rel, weight_col, max_depth)` | Weighted shortest path |
| `ray_louvain(g, rel, max_iter)` | Louvain community detection |
| `ray_degree_cent(g, rel)` | Degree centrality |
| `ray_topsort(g, rel)` | Topological sort (Kahn's) |
| `ray_dfs(g, src, rel, max_depth)` | Depth-first search |
| `ray_astar(g, src, dst, rel, weight, lat, lon, props, max_depth)` | A* shortest path |
| `ray_k_shortest(g, src, dst, rel, weight_col, k)` | Yen's k-shortest paths |
| `ray_cluster_coeff(g, rel)` | Clustering coefficients |
| `ray_random_walk(g, src, rel, walk_length)` | Random walk traversal |
| `ray_betweenness(g, rel, sample_size)` | Betweenness centrality (Brandes) |
| `ray_closeness(g, rel, sample_size)` | Closeness centrality |
| `ray_mst(g, rel, weight_col)` | Minimum spanning forest (Kruskal) |

## Optimizer & Executor

### ray_optimize

Runs the multi-pass optimizer on the DAG rooted at `root`: type inference, constant folding, sideways information passing, factorize, predicate pushdown, filter reorder, projection pushdown, partition pruning, fusion, dead code elimination. Returns the optimized root node (may differ from input).

```c
ray_op_t* ray_optimize(ray_graph_t* g, ray_op_t* root);
```

### ray_execute

Executes the DAG from the given root node. Processes data in 1024-element morsels through fused bytecode pipelines. Returns a `ray_t*` result (vector or table). Returns an error object on failure — check with `RAY_IS_ERR()`. The caller owns the result and must release it.

```c
ray_t* ray_execute(ray_graph_t* g, ray_op_t* root);

ray_t* result = ray_execute(g, ray_optimize(g, root));
if (RAY_IS_ERR(result)) {
    /* handle error */
    ray_release(result);
}
```

## CSR / Relationship API { #csr }

Build, save, load, and query double-indexed CSR edge indices for graph traversal.

### ray_rel_build

Builds a CSR relationship from a table with a foreign-key column. Creates forward index only. Set `sort_targets=true` for sorted adjacency lists (required for WCO join).

```c
ray_rel_t* ray_rel_build(ray_t* from_table, const char* fk_col,
                         int64_t n_target_nodes, bool sort_targets);
```

### ray_rel_from_edges

Builds a double-indexed CSR (forward + reverse) from an edge table with `src_col` and `dst_col`. Specify the number of source and destination nodes explicitly.

```c
ray_rel_t* ray_rel_from_edges(ray_t* edge_table,
    const char* src_col, const char* dst_col,
    int64_t n_src_nodes, int64_t n_dst_nodes,
    bool sort_targets);
```

### ray_rel_save / ray_rel_load / ray_rel_mmap

Persist and load CSR indices as column files in a directory. `ray_rel_mmap` memory-maps the files for zero-copy access.

```c
ray_err_t  ray_rel_save(ray_rel_t* rel, const char* dir);
ray_rel_t* ray_rel_load(const char* dir);
ray_rel_t* ray_rel_mmap(const char* dir);
```

### ray_rel_free

Frees a relationship and both its CSR indices (offsets, targets, rowmap vectors).

```c
void ray_rel_free(ray_rel_t* rel);
```

## Storage API

Columnar file I/O for vectors, splayed tables, and CSV.

### Column I/O

`ray_col_save` / `ray_col_load` / `ray_col_mmap`. Save a vector to a column file, load it back, or memory-map it for zero-copy reads. The file format includes type, length, attributes, and element data; null state is represented with type-correct sentinel payloads plus the `RAY_ATTR_HAS_NULLS` hint.

```c
ray_err_t ray_col_save(ray_t* vec, const char* path);
ray_t*    ray_col_load(const char* path);
ray_t*    ray_col_mmap(const char* path);
```

### Splayed Tables

`ray_splay_save` / `ray_splay_load`. Save a table as a directory of column files (one per column) plus a symbol table. Load reconstructs the table from the directory.

```c
ray_err_t ray_splay_save(ray_t* tbl, const char* dir,
                        const char* sym_path);
ray_t*    ray_splay_load(const char* dir,
                        const char* sym_path);
```

### CSV I/O

`ray_read_csv` / `ray_read_csv_opts` / `ray_write_csv`. `ray_read_csv` loads a CSV file with automatic type inference, parallel parsing, and null handling. `ray_read_csv_opts` allows a custom delimiter, header flag, and per-column type overrides (`col_types` is an array of `n_types` type codes; pass `NULL`/`0` to keep automatic inference). `ray_write_csv` writes a table to CSV.

```c
ray_t*    ray_read_csv(const char* path);
ray_t*    ray_read_csv_opts(const char* path,
              char delimiter, bool header,
              const int8_t* col_types, int32_t n_types);
ray_err_t ray_write_csv(ray_t* table, const char* path);
```

## Datalog API

Build, stratify, and evaluate Datalog programs over Rayforce tables. Include `"ops/datalog.h"` for all Datalog types and functions.

### dl_program_new / dl_program_free

Create a new empty Datalog program, or free one and release all owned tables.

```c
dl_program_t* dl_program_new(void);
void dl_program_free(dl_program_t* prog);
```

### dl_add_edb

Register an extensional (base fact) relation backed by an existing columnar table. Column names are auto-generated as `"{name}__c0"`, `"{name}__c1"`, … (prefixed with the relation name to avoid collisions in joins). Returns the relation index.

```c
int dl_add_edb(dl_program_t* prog, const char* name,
              ray_t* table, int arity);
```

### dl_add_rule

Add a rule to the program. The rule struct is copied. Returns the rule index. Use the rule builder helpers (`dl_rule_init`, `dl_rule_add_atom`, etc.) to construct rules.

```c
int dl_add_rule(dl_program_t* prog, const dl_rule_t* rule);
```

### dl_stratify

Compute stratification (topological sort of the negation dependency graph). Returns 0 on success, −1 if the program has an unstratifiable negation cycle.

```c
int dl_stratify(dl_program_t* prog);
```

### dl_eval

Evaluate the program to fixpoint using semi-naive evaluation. Returns 0 on success, −1 on error. After evaluation, derived relations are populated and queryable.

```c
int dl_eval(dl_program_t* prog);
```

### dl_query

Query the result of a derived relation after evaluation. Returns the backing `ray_t*` table. The caller does **not** own the result — do not release it.

```c
ray_t* dl_query(dl_program_t* prog, const char* pred_name);
```

### Rule Builder Helpers

`dl_rule_init` / `dl_rule_add_atom` / `dl_body_set_var` / `dl_body_set_const`. Build rules programmatically: initialize a rule with a head predicate, add positive body atoms, and bind arguments to variables or constants.

```c
void dl_rule_init(dl_rule_t* rule, const char* head_pred, int head_arity);
void dl_rule_head_var(dl_rule_t* rule, int pos, int var_idx);
int  dl_rule_add_atom(dl_rule_t* rule, const char* pred, int arity);
void dl_body_set_var(dl_rule_t* rule, int body_idx, int pos, int var_idx);
void dl_body_set_const(dl_rule_t* rule, int body_idx, int pos, int64_t val);
int  dl_rule_add_neg(dl_rule_t* rule, const char* pred, int arity);
int  dl_rule_add_cmp_const(dl_rule_t* rule, int cmp_op,
                            int lhs_var, int64_t rhs_val);
```

#### Datalog example: transitive closure

```c
#include <rayforce.h>
#include "ops/ops.h"
#include "mem/heap.h"
#include "ops/datalog.h"

int main(void) {
    ray_heap_init();
    ray_sym_init();

    /* Build edge table: 0->1, 1->2, 2->3 */
    ray_t* c0 = ray_vec_from_raw(RAY_I64, (int64_t[]){0,1,2}, 3);
    ray_t* c1 = ray_vec_from_raw(RAY_I64, (int64_t[]){1,2,3}, 3);
    ray_t* edges = ray_table_new(2);
    edges = ray_table_add_col(edges, ray_sym_intern("c0", 2), c0);
    edges = ray_table_add_col(edges, ray_sym_intern("c1", 2), c1);

    /* Create Datalog program */
    dl_program_t* prog = dl_program_new();
    dl_add_edb(prog, "edge", edges, 2);

    /* Rule 1: reach(X, Y) :- edge(X, Y). */
    dl_rule_t r1;
    dl_rule_init(&r1, "reach", 2);
    dl_rule_head_var(&r1, 0, 0);  /* X */
    dl_rule_head_var(&r1, 1, 1);  /* Y */
    int b = dl_rule_add_atom(&r1, "edge", 2);
    dl_body_set_var(&r1, b, 0, 0);
    dl_body_set_var(&r1, b, 1, 1);
    r1.n_vars = 2;
    dl_add_rule(prog, &r1);

    /* Rule 2: reach(X, Z) :- reach(X, Y), edge(Y, Z). */
    dl_rule_t r2;
    dl_rule_init(&r2, "reach", 2);
    dl_rule_head_var(&r2, 0, 0);  /* X */
    dl_rule_head_var(&r2, 1, 2);  /* Z */
    int b1 = dl_rule_add_atom(&r2, "reach", 2);
    dl_body_set_var(&r2, b1, 0, 0);
    dl_body_set_var(&r2, b1, 1, 1);
    int b2 = dl_rule_add_atom(&r2, "edge", 2);
    dl_body_set_var(&r2, b2, 0, 1);
    dl_body_set_var(&r2, b2, 1, 2);
    r2.n_vars = 3;
    dl_add_rule(prog, &r2);

    /* Stratify, evaluate, query */
    dl_stratify(prog);
    dl_eval(prog);
    ray_t* result = dl_query(prog, "reach");
    /* result is a table with columns reach__c0, reach__c1:
     *   0 | 1
     *   1 | 2
     *   2 | 3
     *   0 | 2
     *   1 | 3
     *   0 | 3  */

    dl_program_free(prog);
    ray_release(c0);
    ray_release(c1);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    return 0;
}
```

## Complete Examples

### Example 1: Filter + Group + Sum

```c
#include <rayforce.h>
#include "ops/ops.h"
#include "mem/heap.h"
#include "io/csv.h"

int main(void) {
    ray_heap_init();
    ray_sym_init();

    ray_t* trades = ray_read_csv("trades.csv");

    /* Build the operation DAG — nothing executes yet */
    ray_graph_t* g = ray_graph_new(trades);

    /* Filter: keep only rows where flag == 0 */
    ray_op_t* flag = ray_scan(g, "flag");
    ray_op_t* pred = ray_eq(g, flag, ray_const_i64(g, 0));

    ray_op_t* region = ray_filter(g, ray_scan(g, "region"), pred);
    ray_op_t* amount = ray_filter(g, ray_scan(g, "amount"), pred);

    /* Group by region, sum amounts */
    ray_op_t* keys[]    = { region };
    uint16_t agg_ops[] = { OP_SUM };
    ray_op_t* agg_ins[] = { amount };
    ray_op_t* grp = ray_group(g, keys, 1, agg_ops, agg_ins, 1);

    /* Optimize (10 passes) and execute */
    ray_t* result = ray_execute(g, ray_optimize(g, grp));

    if (result && !RAY_IS_ERR(result)) ray_release(result);
    ray_graph_free(g);
    ray_release(trades);
    ray_sym_destroy();
    ray_heap_destroy();
    return 0;
}
```

### Example 2: Graph BFS Traversal

```c
#include <rayforce.h>
#include "ops/ops.h"
#include "mem/heap.h"

int main(void) {
    ray_heap_init();
    ray_sym_init();

    /* Build a directed graph: 0->1, 0->2, 1->2, 1->3, 2->3, 3->0 */
    ray_t* src = ray_vec_from_raw(RAY_I64,
                    (int64_t[]){0,0,1,1,2,3}, 6);
    ray_t* dst = ray_vec_from_raw(RAY_I64,
                    (int64_t[]){1,2,2,3,3,0}, 6);

    ray_t* edges = ray_table_new(2);
    edges = ray_table_add_col(edges,
                ray_sym_intern("src", 3), src);
    edges = ray_table_add_col(edges,
                ray_sym_intern("dst", 3), dst);
    ray_release(src);
    ray_release(dst);

    /* Double-indexed CSR (forward + reverse) */
    ray_rel_t* rel = ray_rel_from_edges(edges,
                        "src", "dst", 4, 4, true);

    /* Start at node 0, BFS 1..3 hops forward */
    ray_t* start = ray_vec_from_raw(RAY_I64,
                        (int64_t[]){0}, 1);
    ray_t* nodes = ray_table_new(1);
    nodes = ray_table_add_col(nodes,
                ray_sym_intern("id", 2), start);
    ray_release(start);

    ray_graph_t* g = ray_graph_new(nodes);
    ray_op_t* reach = ray_var_expand(g,
        ray_scan(g, "id"), rel, 0, 1, 3, false);

    ray_t* result = ray_execute(g, ray_optimize(g, reach));

    /*  src | dst | depth
     *  ----|-----|------
     *    0 |   1 |     1
     *    0 |   2 |     1
     *    0 |   3 |     2  */

    if (result && !RAY_IS_ERR(result)) ray_release(result);
    ray_graph_free(g);
    ray_rel_free(rel);
    ray_release(edges);
    ray_release(nodes);
    ray_sym_destroy();
    ray_heap_destroy();
    return 0;
}
```

### Example 3: Join Two Tables

```c
#include <rayforce.h>
#include "ops/ops.h"
#include "mem/heap.h"
#include "io/csv.h"

int main(void) {
    ray_heap_init();
    ray_sym_init();

    ray_t* orders = ray_read_csv("orders.csv");
    ray_t* custs  = ray_read_csv("customers.csv");

    ray_graph_t* g = ray_graph_new(orders);

    /* Inject both tables into the DAG */
    ray_op_t* lo = ray_const_table(g, orders);
    ray_op_t* ro = ray_const_table(g, custs);

    /* Inner join on customer_id */
    ray_op_t* lk[] = { ray_scan(g, "customer_id") };
    ray_op_t* rk[] = { ray_scan(g, "customer_id") };
    ray_op_t* joined = ray_join(g, lo, lk, ro, rk, 1, 0);

    ray_t* result = ray_execute(g, ray_optimize(g, joined));

    /*  customer_id | amount | name
     *  ------------|--------|--------
     *            1 |    250 | Alice
     *            2 |    180 | Bob
     *            2 |    340 | Bob
     *            3 |    120 | Charlie  */

    if (result && !RAY_IS_ERR(result)) ray_release(result);
    ray_graph_free(g);
    ray_release(orders);
    ray_release(custs);
    ray_sym_destroy();
    ray_heap_destroy();
    return 0;
}
```
