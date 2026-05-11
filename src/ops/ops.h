/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.

 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:

 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.

 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

#ifndef RAY_OPS_H
#define RAY_OPS_H

#include <rayforce.h>
#include "store/hnsw.h"  /* ray_hnsw_metric_t, ray_hnsw_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ===== Internal Type Constants ===== */

#define RAY_SEL       14   /* selection bitmap (lazy filter) */

/* Lazy DAG handle (atom-only; stored inline in nullmap region) */
#define RAY_LAZY      104

/* ===== Forward Declarations (internal types) ===== */

typedef struct ray_pool      ray_pool_t;
typedef struct ray_csr       ray_csr_t;
typedef struct ray_rel       ray_rel_t;
typedef struct ray_hnsw      ray_hnsw_t;

/* ===== Lazy DAG Handle Accessors ===== */

typedef struct ray_graph ray_graph_t;
typedef struct ray_op    ray_op_t;

static inline bool ray_is_lazy(ray_t* x) {
    return x && !RAY_IS_ERR(x) && x->type == RAY_LAZY;
}

ray_t*    ray_lazy_materialize(ray_t* val);

/* ===== Cancel API ===== */

void     ray_cancel(void);

/* ===== Parted Types ===== */

#define RAY_PARTED_BASE   32
#define RAY_MAPCOMMON     64   /* virtual partition column */

/* MAPCOMMON inferred sub-types (stored in attrs field) */
#define RAY_MC_SYM    0   /* opaque partition key strings */
#define RAY_MC_DATE   1   /* YYYY.MM.DD partition directories */
#define RAY_MC_I64    2   /* pure integer partition keys */

#define RAY_IS_PARTED(t)       ((t) >= RAY_PARTED_BASE && (t) < RAY_MAPCOMMON)
#define RAY_PARTED_BASETYPE(t) ((t) - RAY_PARTED_BASE)

/* ===== Morsel Constants ===== */

#define RAY_MORSEL_ELEMS  1024

/* ===== Slab Cache Constants ===== */

#define RAY_SLAB_CACHE_SIZE  64
#define RAY_SLAB_ORDERS      5

/* ===== Heap Allocator Constants ===== */

#define RAY_ORDER_MIN  6
#define RAY_ORDER_MAX  30

/* ===== Parallel Threshold ===== */

#define RAY_PARALLEL_THRESHOLD  (64 * RAY_MORSEL_ELEMS)
#define RAY_DISPATCH_MORSELS    8

/* Radix-partitioned hash join tuning.
 * L2_TARGET: per-partition HT working set limit (tuned for L1d/L2).     */
#define RAY_JOIN_L2_TARGET   (256 * 1024)   /* target partition HT size in bytes */
#define RAY_JOIN_MIN_RADIX   2              /* min radix bits (4 partitions)   */
#define RAY_JOIN_MAX_RADIX   14             /* max radix bits (16K partitions) */

/* ===== Operation Graph ===== */

/* Opcodes — Sources */
#define OP_SCAN          1
#define OP_CONST         2

/* Opcodes — Unary element-wise (fuseable) */
#define OP_NEG          10
#define OP_ABS          11
#define OP_NOT          12
#define OP_SQRT         13
#define OP_LOG          14
#define OP_EXP          15
#define OP_CEIL         16
#define OP_FLOOR        17
#define OP_ISNULL       18
#define OP_CAST         19
#define OP_ROUND         9   /* unary element-wise round */

/* Opcodes — Binary element-wise (fuseable) */
#define OP_ADD          20
#define OP_SUB          21
#define OP_MUL          22
#define OP_DIV          23
#define OP_MOD          24
#define OP_EQ           25
#define OP_NE           26
#define OP_LT           27
#define OP_LE           28
#define OP_GT           29
#define OP_GE           30
#define OP_AND          31
#define OP_OR           32
#define OP_MIN2         33
#define OP_MAX2         34
#define OP_IF           35
#define OP_LIKE         36
#define OP_UPPER        37
#define OP_LOWER        38
#define OP_STRLEN       39
#define OP_SUBSTR       40
#define OP_REPLACE      41
#define OP_TRIM         42
#define OP_CONCAT       43
#define OP_EXTRACT      45
#define OP_DATE_TRUNC   46
#define OP_IN           47   /* binary: col in set_vec -> BOOL */
#define OP_NOT_IN       48   /* binary: col not in set_vec -> BOOL */
#define OP_IDIV         49   /* binary: integer floor division -> I64 */

/* EXTRACT / DATE_TRUNC field identifiers */
#define RAY_EXTRACT_YEAR    0
#define RAY_EXTRACT_MONTH   1
#define RAY_EXTRACT_DAY     2
#define RAY_EXTRACT_HOUR    3
#define RAY_EXTRACT_MINUTE  4
#define RAY_EXTRACT_SECOND  5
#define RAY_EXTRACT_DOW     6
#define RAY_EXTRACT_DOY     7
#define RAY_EXTRACT_EPOCH   8

/* Opcodes — Reductions (pipeline breakers) */
#define OP_SUM          50
#define OP_PROD         51
#define OP_MIN          52
#define OP_MAX          53
#define OP_COUNT        54
#define OP_AVG          55
#define OP_FIRST        56
#define OP_LAST         57
#define OP_COUNT_DISTINCT 58
#define OP_STDDEV       59

/* Opcodes — Structural (pipeline breakers) */
#define OP_FILTER       60
#define OP_SORT         61
#define OP_GROUP        62
#define OP_JOIN         63
#define OP_WINDOW_JOIN  64
/* Fused filter+group: predicate evaluated on each morsel and group HT
 * updated inline.  Produced by the planner for `(select … where … by …)`
 * when the predicate shape is supported (see src/ops/fused_group.c). */
#define OP_FILTERED_GROUP 65
#define OP_SELECT       66
#define OP_HEAD         67
#define OP_TAIL         68

/* Opcodes — Window */
#define OP_WINDOW       72

/* Opcodes — Statistical aggregates */
#define OP_STDDEV_POP   73
#define OP_VAR          74
#define OP_VAR_POP      75
#define OP_ILIKE        76
#define OP_PIVOT        77   /* single-pass pivot table            */
#define OP_ANTIJOIN     78   /* anti-semi-join (left rows with no right match) */
#define OP_PEARSON_CORR 79   /* Pearson correlation per group (binary input) */
#define OP_MEDIAN       88   /* exact median per group (bucket-scatter + quickselect) */

/* Opcodes — Graph */
#define OP_EXPAND        80   /* 1-hop CSR neighbor expansion       */
#define OP_VAR_EXPAND    81   /* variable-length BFS/DFS            */
#define OP_SHORTEST_PATH 82   /* BFS shortest path                  */
#define OP_WCO_JOIN      83   /* worst-case optimal join (LFTJ)     */
#define OP_PAGERANK        84   /* iterative PageRank                 */
#define OP_CONNECTED_COMP  85   /* connected components (label prop)  */
#define OP_DIJKSTRA        86   /* weighted shortest path (Dijkstra)  */
#define OP_LOUVAIN         87   /* community detection (Louvain)      */

/* Opcodes — Graph algorithms (batch 1) */
#define OP_DEGREE_CENT     92   /* degree centrality                  */
#define OP_TOPSORT         93   /* topological sort (Kahn's)          */
#define OP_DFS             94   /* depth-first search traversal       */

/* Opcodes — Graph algorithms (batch 2) */
#define OP_ASTAR           95   /* A* shortest path (coordinate heuristic) */
#define OP_K_SHORTEST      96   /* Yen's k-shortest paths                 */
#define OP_CLUSTER_COEFF   97   /* clustering coefficients                */
#define OP_RANDOM_WALK     98   /* random walk traversal                  */
#define OP_BETWEENNESS     99   /* betweenness centrality (Brandes)       */
#define OP_CLOSENESS      100   /* closeness centrality                   */
#define OP_MST            101   /* minimum spanning forest (Kruskal)      */

/* Opcodes — Vector search */
#define OP_ANN_RERANK     102   /* index-backed ANN over filtered source  */
#define OP_KNN_RERANK     103   /* brute-force KNN over filtered source   */
#define OP_DISTINCT       104   /* unique elements (preserves first occurrence) */
#define OP_ASC            105   /* sort vector ascending                        */
#define OP_DESC           106   /* sort vector descending                       */
#define OP_REVERSE        107   /* reverse vector order                         */

/* Opcodes — Misc */
#define OP_ALIAS        70
#define OP_MATERIALIZE  71

/* Window function kinds (stored in func_kinds[]) */
#define RAY_WIN_ROW_NUMBER    0
#define RAY_WIN_RANK          1
#define RAY_WIN_DENSE_RANK    2
#define RAY_WIN_NTILE         3
#define RAY_WIN_SUM           4
#define RAY_WIN_AVG           5
#define RAY_WIN_MIN           6
#define RAY_WIN_MAX           7
#define RAY_WIN_COUNT         8
#define RAY_WIN_LAG           9
#define RAY_WIN_LEAD         10
#define RAY_WIN_FIRST_VALUE  11
#define RAY_WIN_LAST_VALUE   12
#define RAY_WIN_NTH_VALUE   13

/* Frame types */
#define RAY_FRAME_ROWS    0
#define RAY_FRAME_RANGE   1

/* Frame bounds */
#define RAY_BOUND_UNBOUNDED_PRECEDING  0
#define RAY_BOUND_N_PRECEDING          1
#define RAY_BOUND_CURRENT_ROW          2
#define RAY_BOUND_N_FOLLOWING          3
#define RAY_BOUND_UNBOUNDED_FOLLOWING  4

/* Op flags */
#define OP_FLAG_FUSED        0x01
#define OP_FLAG_DEAD         0x02

/* Operation node (32 bytes, fits one cache line) */
typedef struct ray_op {
    uint16_t       opcode;     /* OP_ADD, OP_SCAN, OP_FILTER, etc. */
    uint8_t        arity;      /* 0, 1, or 2 */
    uint8_t        flags;      /* FUSED, DEAD */
    int8_t         out_type;   /* inferred output type */
    uint8_t        pad[3];
    uint32_t       id;         /* unique node ID */
    uint32_t       est_rows;   /* estimated row count */
    struct ray_op*  inputs[2];  /* NULL if unused */
} ray_op_t;

/* Extended operation node for N-ary ops (heap-allocated, variable size) */
typedef struct ray_op_ext {
    ray_op_t base;              /* 32 bytes standard node */
    union {
        ray_t*   literal;       /* OP_CONST: inline literal value */
        int64_t sym;           /* OP_SCAN: column name symbol ID */
        struct {               /* OP_GROUP: group-by specification */
            ray_op_t**  keys;
            uint8_t    n_keys;
            uint8_t    n_aggs;
            uint16_t*  agg_ops;
            ray_op_t**  agg_ins;
            /* Optional second input per agg — non-NULL only for binary
             * aggregators (currently: OP_PEARSON_CORR). NULL for all
             * unary aggs and for the whole pointer when no binary agg
             * is present in this group. */
            ray_op_t**  agg_ins2;
        };
        struct {               /* OP_SORT: multi-column sort */
            ray_op_t**  columns;
            uint8_t*   desc;
            uint8_t*   nulls_first; /* 1=nulls first, 0=nulls last */
            uint8_t    n_cols;
        } sort;
        struct {               /* OP_JOIN: join specification */
            ray_op_t**  left_keys;
            ray_op_t**  right_keys;
            uint8_t    n_join_keys;
            uint8_t    join_type;  /* 0=inner, 1=left, 2=full, 3=anti */
        } join;
        struct {               /* OP_WINDOW_JOIN: ASOF join */
            ray_op_t*   time_key;      /* time/ordered key column */
            ray_op_t**  eq_keys;       /* equality partition keys */
            uint8_t    n_eq_keys;     /* number of equality keys */
            uint8_t    join_type;     /* 0=inner, 1=left outer */
        } asof;
        struct {               /* OP_WINDOW: window functions */
            ray_op_t**  part_keys;
            ray_op_t**  order_keys;
            uint8_t*   order_descs;
            ray_op_t**  func_inputs;
            uint8_t*   func_kinds;    /* RAY_WIN_ROW_NUMBER etc. */
            int64_t*   func_params;   /* NTILE(n), LAG offset, etc. */
            uint8_t    n_part_keys;
            uint8_t    n_order_keys;
            uint8_t    n_funcs;
            uint8_t    frame_type;    /* RAY_FRAME_ROWS / RAY_FRAME_RANGE */
            uint8_t    frame_start;   /* RAY_BOUND_* */
            uint8_t    frame_end;     /* RAY_BOUND_* */
            int64_t    frame_start_n;
            int64_t    frame_end_n;
        } window;
        struct {  /* OP_EXPAND / OP_VAR_EXPAND / OP_SHORTEST_PATH / graph algos */
            void*     rel;            /* ray_rel_t* (opaque to public header) */
            void*     sip_sel;        /* ray_t* RAY_SEL bitmap for SIP source-side skip */
            uint8_t   direction;      /* 0=fwd, 1=rev, 2=both */
            uint8_t   min_depth;
            uint8_t   max_depth;
            uint8_t   path_tracking;
            uint8_t   factorized;     /* 1 = emit factorized output (fvec) */
            uint16_t  max_iter;       /* PageRank/Louvain iterations  */
            double    damping;        /* PageRank damping factor      */
            int64_t   weight_col_sym; /* Dijkstra/Astar/Yen weight column   */
            int64_t   coord_col_syms[2]; /* A*: lat/lon property column names */
            void*     node_props;       /* ray_t* node property table (A*: coords) */
        } graph;
        struct {  /* OP_WCO_JOIN */
            void**    rels;           /* ray_rel_t** array */
            uint8_t   n_rels;
            uint8_t   n_vars;
        } wco;
        struct {  /* OP_ANN_RERANK / OP_KNN_RERANK */
            void*     hnsw_idx;       /* ray_hnsw_t* for ANN; NULL for KNN */
            int64_t   col_sym;        /* sym id of column for KNN; 0 for ANN */
            float*    query_vec;      /* caller-owned */
            int32_t   dim;
            int32_t   metric;         /* ray_hnsw_metric_t — KNN variant only */
            int64_t   k;              /* target result count from `take` */
            int32_t   ef_search;      /* ANN only */
        } rerank;
        struct {  /* OP_PIVOT */
            ray_op_t**  index_cols;   /* OP_SCAN nodes for index columns */
            ray_op_t*   pivot_col;    /* OP_SCAN node for pivot column */
            ray_op_t*   value_col;    /* OP_SCAN node for value column */
            uint16_t    agg_op;       /* OP_SUM, OP_AVG, etc. */
            uint8_t     n_index;      /* number of index columns */
        } pivot;
    };
    uint64_t* seg_mask;   /* partition pruning bitmap (NULL = all active) */
    int64_t   seg_mask_count; /* number of partitions the mask covers */
} ray_op_ext_t;

/* Operation graph */
typedef struct ray_graph {
    ray_op_t*       nodes;       /* array of op nodes (malloc'd) */
    uint32_t       node_count;  /* number of nodes */
    uint32_t       node_cap;    /* allocated capacity */
    ray_t*          table;       /* bound table (provides columns for OP_SCAN) */
    ray_t**         tables;      /* table registry (indexed by table_id) */
    uint16_t       n_tables;    /* number of registered tables */
    ray_op_ext_t**  ext_nodes;   /* tracked extended nodes for cleanup */
    uint32_t       ext_count;   /* number of extended nodes */
    uint32_t       ext_cap;     /* capacity of ext_nodes array */
    ray_t*          selection;   /* RAY_SEL bitmap — lazy filter (NULL = all pass) */

    /* Compile-time local env for lambda / let inlining in
     * compile_expr_dag (src/ops/query.c).  Stack of
     * {formal_sym_id → node_id}.  Pushed on lambda call / let
     * entry, popped on exit.  Looked up BEFORE ray_scan so
     * formals shadow column names naturally.
     *
     * Stores node IDs (uint32_t), not raw ray_op_t* — the
     * g->nodes array is dynamically resized, so any realloc
     * between push and lookup would dangle stored pointers.
     * Lookup re-resolves &g->nodes[id] on every call. */
    struct {
        int64_t    sym;
        uint32_t   node_id;
    } cexpr_env[32];
    int             cexpr_env_top;
} ray_graph_t;

/* ===== Morsel Iterator ===== */

typedef struct {
    ray_t*    vec;          /* source vector */
    int64_t  offset;       /* current position (element index) */
    int64_t  len;          /* total length of vector */
    uint32_t elem_size;    /* bytes per element */
    int64_t  morsel_len;   /* elements in current morsel (<=RAY_MORSEL_ELEMS) */
    void*    morsel_ptr;   /* pointer to current morsel data */
    uint8_t* null_bits;    /* current morsel null bitmap (or NULL) */
} ray_morsel_t;

/* ===== Selection Bitmap (RAY_SEL) ===== */

/* Segment flags — one per morsel (RAY_MORSEL_ELEMS rows) */
#define RAY_SEL_NONE  0   /* all bits 0 — skip entire morsel           */
#define RAY_SEL_ALL   1   /* all bits 1 — process without bitmap check */
#define RAY_SEL_MIX   2   /* mixed bits — must check per-row           */

/* Words per morsel segment: 1024 rows / 64 bits = 16 uint64_t */
#define RAY_SEL_WORDS_PER_SEG  (RAY_MORSEL_ELEMS / 64)

/* Inline metadata at ray_data(sel) */
typedef struct {
    int64_t   total_pass;  /* total passing rows                      */
    uint32_t  n_segs;      /* ceil(nrows / RAY_MORSEL_ELEMS)           */
    uint32_t  _pad;
} ray_sel_meta_t;

/*
 * RAY_SEL block layout (ray_data offset 0):
 *
 *   ray_sel_meta_t  meta        (16 bytes)
 *   uint8_t        seg_flags[] (n_segs, padded to 8-byte alignment)
 *   uint16_t       seg_popcnt[](n_segs, padded to 8-byte alignment)
 *   uint64_t       bits[]      (ceil(nrows/64) words)
 */

static inline ray_sel_meta_t* ray_sel_meta(ray_t* s) {
    return (ray_sel_meta_t*)ray_data(s);
}
static inline uint8_t* ray_sel_flags(ray_t* s) {
    return (uint8_t*)ray_data(s) + sizeof(ray_sel_meta_t);
}
static inline uint16_t* ray_sel_popcnt(ray_t* s) {
    uint32_t n = ray_sel_meta(s)->n_segs;
    return (uint16_t*)(ray_sel_flags(s) + ((n + 7u) & ~7u));
}
static inline uint64_t* ray_sel_bits(ray_t* s) {
    uint32_t n = ray_sel_meta(s)->n_segs;
    uint16_t* pc = ray_sel_popcnt(s);
    return (uint64_t*)(pc + ((n + 3u) & ~3u));
}

/* Bit ops */
#define RAY_SEL_BIT_TEST(bits, r)  ((bits)[(r) >> 6] & (1ULL << ((r) & 63)))
#define RAY_SEL_BIT_SET(bits, r)   ((bits)[(r) >> 6] |= (1ULL << ((r) & 63)))
#define RAY_SEL_BIT_CLR(bits, r)   ((bits)[(r) >> 6] &= ~(1ULL << ((r) & 63)))

/* ===== Executor Pipeline ===== */

typedef struct ray_pipe {
    ray_op_t*          op;            /* operation node */
    struct ray_pipe*   inputs[2];     /* upstream pipes */
    ray_morsel_t       state;         /* current morsel state */
    ray_t*             materialized;  /* materialized intermediate (or NULL) */
    int               spill_fd;      /* file descriptor for spill (-1 if none) */
} ray_pipe_t;

/* ===== Selection API ===== */

ray_t* ray_sel_new(int64_t nrows);              /* all-zero (no rows pass)       */
ray_t* ray_sel_from_pred(ray_t* bool_vec);       /* convert RAY_BOOL vec -> RAY_SEL  */
ray_t* ray_sel_and(ray_t* a, ray_t* b);           /* AND two selections            */
void  ray_sel_recompute(ray_t* sel);             /* rebuild seg_flags + popcounts */

/* ===== Morsel Iterator API ===== */

void ray_morsel_init(ray_morsel_t* m, ray_t* vec);
void ray_morsel_init_range(ray_morsel_t* m, ray_t* vec, int64_t start, int64_t end);
bool ray_morsel_next(ray_morsel_t* m);

/* ===== Operation Graph API ===== */

ray_graph_t* ray_graph_new(ray_t* tbl);
void        ray_graph_free(ray_graph_t* g);

/* Source ops */
ray_op_t* ray_scan(ray_graph_t* g, const char* col_name);
ray_op_t* ray_const_f64(ray_graph_t* g, double val);
ray_op_t* ray_const_i64(ray_graph_t* g, int64_t val);
ray_op_t* ray_const_bool(ray_graph_t* g, bool val);
ray_op_t* ray_const_str(ray_graph_t* g, const char* s, size_t len);
ray_op_t* ray_const_vec(ray_graph_t* g, ray_t* vec);
ray_op_t* ray_const_atom(ray_graph_t* g, ray_t* atom);
ray_op_t* ray_const_table(ray_graph_t* g, ray_t* table);

/* Unary element-wise ops */
ray_op_t* ray_neg(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_abs(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_not(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_sqrt_op(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_log_op(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_exp_op(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_ceil_op(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_floor_op(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_round_op(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_isnull(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_cast(ray_graph_t* g, ray_op_t* a, int8_t target_type);

/* Generic binary op — opcode-driven dispatch, no switch/case */
ray_op_t* ray_binop(ray_graph_t* g, uint16_t opcode, ray_op_t* a, ray_op_t* b);

/* Binary element-wise ops */
ray_op_t* ray_add(ray_graph_t* g, ray_op_t* a, ray_op_t* b);
ray_op_t* ray_sub(ray_graph_t* g, ray_op_t* a, ray_op_t* b);
ray_op_t* ray_mul(ray_graph_t* g, ray_op_t* a, ray_op_t* b);
ray_op_t* ray_div(ray_graph_t* g, ray_op_t* a, ray_op_t* b);
ray_op_t* ray_idiv(ray_graph_t* g, ray_op_t* a, ray_op_t* b);
ray_op_t* ray_mod(ray_graph_t* g, ray_op_t* a, ray_op_t* b);
ray_op_t* ray_eq(ray_graph_t* g, ray_op_t* a, ray_op_t* b);
ray_op_t* ray_ne(ray_graph_t* g, ray_op_t* a, ray_op_t* b);
ray_op_t* ray_lt(ray_graph_t* g, ray_op_t* a, ray_op_t* b);
ray_op_t* ray_le(ray_graph_t* g, ray_op_t* a, ray_op_t* b);
ray_op_t* ray_gt(ray_graph_t* g, ray_op_t* a, ray_op_t* b);
ray_op_t* ray_ge(ray_graph_t* g, ray_op_t* a, ray_op_t* b);
ray_op_t* ray_and(ray_graph_t* g, ray_op_t* a, ray_op_t* b);
ray_op_t* ray_or(ray_graph_t* g, ray_op_t* a, ray_op_t* b);
ray_op_t* ray_min2(ray_graph_t* g, ray_op_t* a, ray_op_t* b);
ray_op_t* ray_max2(ray_graph_t* g, ray_op_t* a, ray_op_t* b);
ray_op_t* ray_in(ray_graph_t* g, ray_op_t* col, ray_op_t* set);
ray_op_t* ray_not_in(ray_graph_t* g, ray_op_t* col, ray_op_t* set);
ray_op_t* ray_if(ray_graph_t* g, ray_op_t* cond, ray_op_t* then_val, ray_op_t* else_val);
ray_op_t* ray_like(ray_graph_t* g, ray_op_t* input, ray_op_t* pattern);
ray_op_t* ray_ilike(ray_graph_t* g, ray_op_t* input, ray_op_t* pattern);
ray_op_t* ray_upper(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_lower(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_strlen(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_substr(ray_graph_t* g, ray_op_t* str, ray_op_t* start, ray_op_t* len);
ray_op_t* ray_replace(ray_graph_t* g, ray_op_t* str, ray_op_t* from, ray_op_t* to);
ray_op_t* ray_trim_op(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_concat(ray_graph_t* g, ray_op_t** args, int n);

/* Date/time extraction and truncation */
ray_op_t* ray_extract(ray_graph_t* g, ray_op_t* col, int64_t field);
ray_op_t* ray_date_trunc(ray_graph_t* g, ray_op_t* col, int64_t field);

/* Reduction ops */
ray_op_t* ray_sum(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_prod(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_min_op(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_max_op(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_count(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_avg(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_first(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_last(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_count_distinct(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_distinct_op(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_asc_op(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_desc_op(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_reverse_op(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_stddev(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_stddev_pop(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_var(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_var_pop(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_pearson_corr(ray_graph_t* g, ray_op_t* x, ray_op_t* y);
ray_op_t* ray_median(ray_graph_t* g, ray_op_t* a);

/* Structural ops */
ray_op_t* ray_filter(ray_graph_t* g, ray_op_t* input, ray_op_t* predicate);
ray_op_t* ray_sort_op(ray_graph_t* g, ray_op_t* table_node,
                     ray_op_t** keys, uint8_t* descs, uint8_t* nulls_first,
                     uint8_t n_cols);
ray_op_t* ray_group(ray_graph_t* g, ray_op_t** keys, uint8_t n_keys,
                   uint16_t* agg_ops, ray_op_t** agg_ins, uint8_t n_aggs);
/* Variant accepting an optional second-input column per agg.  agg_ins2
 * is parallel to agg_ins (length n_aggs); slots are NULL for unary aggs
 * and non-NULL only for binary aggregators (currently OP_PEARSON_CORR). */
ray_op_t* ray_group2(ray_graph_t* g, ray_op_t** keys, uint8_t n_keys,
                     uint16_t* agg_ops, ray_op_t** agg_ins,
                     ray_op_t** agg_ins2, uint8_t n_aggs);
ray_op_t* ray_distinct(ray_graph_t* g, ray_op_t** keys, uint8_t n_keys);
ray_op_t* ray_pivot_op(ray_graph_t* g,
                       ray_op_t** index_cols, uint8_t n_index,
                       ray_op_t* pivot_col,
                       ray_op_t* value_col,
                       uint16_t agg_op);
ray_op_t* ray_join(ray_graph_t* g,
                  ray_op_t* left_table, ray_op_t** left_keys,
                  ray_op_t* right_table, ray_op_t** right_keys,
                  uint8_t n_keys, uint8_t join_type);
ray_op_t* ray_antijoin(ray_graph_t* g,
                      ray_op_t* left_table, ray_op_t** left_keys,
                      ray_op_t* right_table, ray_op_t** right_keys,
                      uint8_t n_keys);
ray_op_t* ray_asof_join(ray_graph_t* g,
                       ray_op_t* left_table, ray_op_t* right_table,
                       ray_op_t* time_key,
                       ray_op_t** eq_keys, uint8_t n_eq_keys,
                       uint8_t join_type);
ray_op_t* ray_window_op(ray_graph_t* g, ray_op_t* table_node,
                       ray_op_t** part_keys, uint8_t n_part,
                       ray_op_t** order_keys, uint8_t* order_descs, uint8_t n_order,
                       uint8_t* func_kinds, ray_op_t** func_inputs,
                       int64_t* func_params, uint8_t n_funcs,
                       uint8_t frame_type, uint8_t frame_start, uint8_t frame_end,
                       int64_t frame_start_n, int64_t frame_end_n);
ray_op_t* ray_select_op(ray_graph_t* g, ray_op_t* input,
                        ray_op_t** cols, uint8_t n_cols);
ray_op_t* ray_head(ray_graph_t* g, ray_op_t* input, int64_t n);
ray_op_t* ray_tail(ray_graph_t* g, ray_op_t* input, int64_t n);
ray_op_t* ray_alias(ray_graph_t* g, ray_op_t* input, const char* name);
ray_op_t* ray_materialize(ray_graph_t* g, ray_op_t* input);

/* ===== Graph Ops ===== */

/* Multi-table support */
uint16_t ray_graph_add_table(ray_graph_t* g, ray_t* table);
ray_op_t* ray_scan_table(ray_graph_t* g, uint16_t table_id, const char* col_name);

/* Graph traversal */
ray_op_t* ray_expand(ray_graph_t* g, ray_op_t* src_nodes,
                    ray_rel_t* rel, uint8_t direction);
ray_op_t* ray_var_expand(ray_graph_t* g, ray_op_t* start_nodes,
                        ray_rel_t* rel, uint8_t direction,
                        uint8_t min_depth, uint8_t max_depth,
                        bool track_path);
ray_op_t* ray_shortest_path(ray_graph_t* g, ray_op_t* src, ray_op_t* dst,
                           ray_rel_t* rel, uint8_t max_depth);
ray_op_t* ray_wco_join(ray_graph_t* g,
                      ray_rel_t** rels, uint8_t n_rels,
                      uint8_t n_vars);

/* Graph algorithms */
ray_op_t* ray_pagerank(ray_graph_t* g, ray_rel_t* rel,
                      uint16_t max_iter, double damping);
ray_op_t* ray_connected_comp(ray_graph_t* g, ray_rel_t* rel);
ray_op_t* ray_dijkstra(ray_graph_t* g, ray_op_t* src, ray_op_t* dst,
                      ray_rel_t* rel, const char* weight_col,
                      uint8_t max_depth);
ray_op_t* ray_louvain(ray_graph_t* g, ray_rel_t* rel,
                     uint16_t max_iter);
ray_op_t* ray_degree_cent(ray_graph_t* g, ray_rel_t* rel);
ray_op_t* ray_topsort(ray_graph_t* g, ray_rel_t* rel);
ray_op_t* ray_dfs(ray_graph_t* g, ray_op_t* src, ray_rel_t* rel, uint8_t max_depth);
ray_op_t* ray_astar(ray_graph_t* g, ray_op_t* src, ray_op_t* dst,
                  ray_rel_t* rel, const char* weight_col,
                  const char* lat_col, const char* lon_col,
                  ray_t* node_props, uint8_t max_depth);
ray_op_t* ray_k_shortest(ray_graph_t* g, ray_op_t* src, ray_op_t* dst,
                       ray_rel_t* rel, const char* weight_col, uint16_t k);
ray_op_t* ray_cluster_coeff(ray_graph_t* g, ray_rel_t* rel);
ray_op_t* ray_random_walk(ray_graph_t* g, ray_op_t* src, ray_rel_t* rel,
                        uint16_t walk_length);
ray_op_t* ray_betweenness(ray_graph_t* g, ray_rel_t* rel, uint16_t sample_size);
ray_op_t* ray_closeness(ray_graph_t* g, ray_rel_t* rel, uint16_t sample_size);
ray_op_t* ray_mst(ray_graph_t* g, ray_rel_t* rel, const char* weight_col);

/* Rerank ops: consume a filtered source table and return top-K nearest rows
 * (source columns + _dist appended).  Used by `select ... nearest ... take`. */
ray_op_t* ray_ann_rerank(ray_graph_t* g, ray_op_t* src,
                         ray_hnsw_t* idx, const float* query_vec,
                         int32_t dim, int64_t k, int32_t ef_search);
ray_op_t* ray_knn_rerank(ray_graph_t* g, ray_op_t* src,
                         int64_t col_sym, const float* query_vec,
                         int32_t dim, int64_t k, ray_hnsw_metric_t metric);

/* CSR / Relationship API */
ray_rel_t* ray_rel_build(ray_t* from_table, const char* fk_col,
                         int64_t n_target_nodes, bool sort_targets);
ray_rel_t* ray_rel_from_edges(ray_t* edge_table,
                             const char* src_col, const char* dst_col,
                             int64_t n_src_nodes, int64_t n_dst_nodes,
                             bool sort_targets);
ray_err_t  ray_rel_save(ray_rel_t* rel, const char* dir);
ray_rel_t* ray_rel_load(const char* dir);
ray_rel_t* ray_rel_mmap(const char* dir);
void      ray_rel_set_props(ray_rel_t* rel, ray_t* props);
void      ray_rel_free(ray_rel_t* rel);
const int64_t* ray_rel_neighbors(ray_rel_t* rel, int64_t node,
                                uint8_t direction, int64_t* out_count);
int64_t   ray_rel_n_nodes(ray_rel_t* rel, uint8_t direction);

/* ===== Optimizer API ===== */

ray_op_t* ray_optimize(ray_graph_t* g, ray_op_t* root);
void     ray_fuse_pass(ray_graph_t* g, ray_op_t* root);

/* ===== Plan Printer ===== */

const char* ray_opcode_name(uint16_t op);
void ray_graph_dump(ray_graph_t* g, ray_op_t* root, void* out);

/* ===== Sort API ===== */

/* Sort columns and return index array (I64 vector of sorted indices).
 * Uses parallel radix sort for numerics, merge sort for strings/symbols.
 * descs/nulls_first may be NULL (all-asc / nulls-last default). */
ray_t* ray_sort_indices(ray_t** cols, uint8_t* descs, uint8_t* nulls_first,
                        uint8_t n_cols, int64_t nrows);

/* Top-K bounded-heap path: returns a new K-row table of `tbl` ordered by
 * `col` in the requested direction.  Returns NULL when the input doesn't
 * fit the single-key fast path (unsupported type, K ≥ nrows, etc.) so
 * the caller can fall back to a full sort.  Skips the full O(n log n)
 * sort entirely — selection runs in O(n log K + K log K). */
ray_t* ray_topk_table(ray_t* tbl, ray_t* col, uint8_t desc, uint8_t nf,
                      int64_t k);

/* Multi-key variant of ray_topk_table: bounded-heap selection on n_keys
 * sort columns with per-key direction / nulls-first.  Same fallback
 * contract — returns NULL when the inputs don't fit the fast path. */
ray_t* ray_topk_table_multi(ray_t* tbl, ray_t** key_cols, uint8_t* descs,
                            uint8_t* nfs, uint8_t n_keys, int64_t k);

/* ===== Executor API ===== */

ray_t* ray_execute(ray_graph_t* g, ray_op_t* root);

/* ===== Lazy DAG Handle (Internal) ===== */

#define RAY_LAZY_GRAPH(p) (*(ray_graph_t**)((p)->nullmap))
#define RAY_LAZY_OP(p)    (*(ray_op_t**)(((p)->nullmap) + 8))

ray_op_t* ray_graph_input_vec(ray_graph_t* g, ray_t* vec);
ray_t*    ray_lazy_wrap(ray_graph_t* g, ray_op_t* op);
ray_t*    ray_lazy_append(ray_t* lazy, uint16_t opcode);

#ifdef __cplusplus
}
#endif

#endif /* RAY_OPS_H */
