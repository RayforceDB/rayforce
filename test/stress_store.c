/*
 * stress_store.c — engine for parted/splayed shared-symfile stress tests.
 * Shadow model uses plain malloc/free so the oracle never depends on the
 * Rayforce heap under test.
 */

#include "stress_store.h"
#include "store/splay.h"
#include "store/part.h"
#include "table/sym.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>  /* getpid — per-process scratch paths */

const char* stress_db_path(const char* name) {
    static char buf[256];
    snprintf(buf, sizeof(buf), "/tmp/rayforce_stress_%s.%d", name, (int)getpid());
    return buf;
}

/* ---- prng (xorshift64*) ------------------------------------------------ */

uint64_t stress_rand(stress_ctx_t* c) {
    uint64_t x = c->rng;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    c->rng = x;
    return x * 0x2545F4914F6CDD1DULL;
}

/* ---- op log + failure dump --------------------------------------------- */

static void op_logv(stress_ctx_t* c, const char* fmt, va_list ap) {
    if (!c->oplog) return;
    char* slot = c->oplog[c->oplog_len % STRESS_OPLOG_CAP];
    vsnprintf(slot, 128, fmt, ap);
    c->oplog_len++;
}

static void op_logf(stress_ctx_t* c, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    op_logv(c, fmt, ap);
    va_end(ap);
}

/* exported variadic wrapper over the op log for the eval driver
 * (stress_eval.c logs every generated source line through this). */
void stress_op_logf(stress_ctx_t* c, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    op_logv(c, fmt, ap);
    va_end(ap);
}

void stress_dump_failure(stress_ctx_t* c, const char* fmt, ...) {
    char why[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, sizeof(why), fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n=== STRESS FAILURE: %s\n", why);
    fprintf(stderr, "=== seed=%llu  db=%s (left on disk for inspection)\n",
            (unsigned long long)c->seed, c->db_root);
    int total = c->oplog_len;
    int n     = total < STRESS_OPLOG_CAP ? total : STRESS_OPLOG_CAP;
    fprintf(stderr, "=== op log (last %d of %d ops):\n", n, total);
    for (int i = total - n; i < total; i++)
        fprintf(stderr, "  %5d: %s\n", i, c->oplog[i % STRESS_OPLOG_CAP]);
    c->failed = true;
}

/* ---- shadow row arrays (plain malloc — independent of rayforce heap) ---- */

static bool rows_reserve(stress_rows_t* r, int64_t need) {
    if (need <= r->cap) return true;
    int64_t cap = r->cap ? r->cap : 64;
    while (cap < need) cap *= 2;
    stress_row_t* p = (stress_row_t*)realloc(r->rows,
                                             (size_t)cap * sizeof(stress_row_t));
    if (!p) return false;
    r->rows = p;
    r->cap  = cap;
    return true;
}

bool stress_rows_append(stress_rows_t* r, const stress_row_t* row) {
    if (!rows_reserve(r, r->len + 1)) return false;
    r->rows[r->len++] = *row;
    return true;
}

void stress_rows_trim(stress_rows_t* r, bool tail, int64_t n) {
    if (n > r->len) n = r->len;
    if (n <= 0) return; /* nothing to do; also avoids memmove(NULL, NULL, 0) UB */
    if (!tail)
        memmove(r->rows, r->rows + n,
                (size_t)(r->len - n) * sizeof(stress_row_t));
    r->len -= n;
}

static void rows_free(stress_rows_t* r) {
    free(r->rows);
    r->rows = NULL;
    r->len = r->cap = 0;
}

/* ---- row generation ----------------------------------------------------- */

void stress_gen_row(stress_ctx_t* c, stress_sym_pattern_t pat,
                    stress_row_t* out) {
    memset(out, 0, sizeof(*out));
    if (pat == STRESS_SYMS_NULLS && (stress_rand(c) & 1)) {
        out->ticker[0] = '\0';        /* ordinary empty SYM (sym 0) */
        out->price     = NULL_F64;
        out->qty       = NULL_I64;
        return;
    }
    bool fresh = (pat == STRESS_SYMS_NEW) || c->pool_len == 0 ||
                 ((pat == STRESS_SYMS_MIXED || pat == STRESS_SYMS_NULLS) &&
                  (stress_rand(c) & 1));
    if (fresh) {
        snprintf(out->ticker, STRESS_SYM_MAX, "tkr%06d", c->sym_uniq++);
        memcpy(c->pool[c->pool_len % STRESS_POOL_CAP], out->ticker,
               STRESS_SYM_MAX);
        c->pool_len++;
    } else {
        int span = c->pool_len < STRESS_POOL_CAP ? c->pool_len
                                                 : STRESS_POOL_CAP;
        memcpy(out->ticker, c->pool[stress_rand(c) % (uint64_t)span],
               STRESS_SYM_MAX);
    }
    out->price = (double)(stress_rand(c) % 1000000) / 100.0;
    out->qty   = (int64_t)(stress_rand(c) % 100000);
}

/* ---- paths --------------------------------------------------------------- */

void stress_live_dir(const stress_ctx_t* c, char* buf, size_t n) {
    snprintf(buf, n, "%s/live", c->db_root);
}

void stress_part_dir(const stress_ctx_t* c, int i, char* buf, size_t n) {
    snprintf(buf, n, "%s/%s/hist", c->db_root, c->part_dates[i]);
}

static void rm_rf(const char* path) {
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    (void)!system(cmd);
}

/* ---- ray table <-> shadow rows ------------------------------------------ */

/* Build a fresh in-memory table from a row array.  Interns tickers.
 * Caller releases the returned table. */
static ray_t* build_table_from_rows(const stress_rows_t* rows) {
    int64_t cap   = rows->len > 0 ? rows->len : 1;
    ray_t*  tick  = ray_sym_vec_new(RAY_SYM_W64, cap);
    ray_t*  price = ray_vec_new(RAY_F64, cap);
    ray_t*  qty   = ray_vec_new(RAY_I64, cap);
    if (!tick || !price || !qty || RAY_IS_ERR(tick) || RAY_IS_ERR(price) ||
        RAY_IS_ERR(qty)) {
        if (tick)  ray_release(tick);
        if (price) ray_release(price);
        if (qty)   ray_release(qty);
        return NULL;
    }
    for (int64_t i = 0; i < rows->len; i++) {
        const stress_row_t* r = &rows->rows[i];
        int64_t id = ray_sym_intern(r->ticker, strlen(r->ticker));
        tick  = ray_vec_append(tick, &id);
        price = ray_vec_append(price, &r->price);
        qty   = ray_vec_append(qty, &r->qty);
        if (!tick || RAY_IS_ERR(tick) || !price || RAY_IS_ERR(price) ||
            !qty || RAY_IS_ERR(qty)) {
            if (tick)  ray_release(tick);
            if (price) ray_release(price);
            if (qty)   ray_release(qty);
            return NULL;
        }
    }
    ray_t* tbl = ray_table_new(4);
    tbl = ray_table_add_col(tbl, ray_sym_intern("ticker", 6), tick);
    tbl = ray_table_add_col(tbl, ray_sym_intern("price", 5), price);
    tbl = ray_table_add_col(tbl, ray_sym_intern("qty", 3), qty);
    ray_release(tick);   /* add_col retained its own refs */
    ray_release(price);
    ray_release(qty);
    if (RAY_IS_ERR(tbl)) {
        ray_release(tbl);
        return NULL;
    }
    return tbl;
}

/* Extract a loaded table's content into a plain row array, resolving sym
 * IDs to strings through the column's domain.  Returns false on
 * structural problems (wrong cols, unresolvable sym). */
static bool extract_rows(stress_ctx_t* c, ray_t* tbl, stress_rows_t* out) {
    if (!tbl || RAY_IS_ERR(tbl)) return false;
    if (ray_table_ncols(tbl) != 3) {
        op_logf(c, "extract: ncols=%lld != 3", (long long)ray_table_ncols(tbl));
        return false;
    }
    ray_t* tick  = ray_table_get_col_idx(tbl, 0); /* borrowed */
    ray_t* price = ray_table_get_col_idx(tbl, 1);
    ray_t* qty   = ray_table_get_col_idx(tbl, 2);
    if (!tick || !price || !qty) return false;
    int64_t n = tick->len;
    if (price->len != n || qty->len != n) {
        op_logf(c, "extract: col length mismatch %lld/%lld/%lld",
                (long long)n, (long long)price->len, (long long)qty->len);
        return false;
    }
    if (!rows_reserve(out, n)) return false;
    const double*  pd = (const double*)ray_data(price);
    const int64_t* qd = (const int64_t*)ray_data(qty);
    for (int64_t i = 0; i < n; i++) {
        stress_row_t* r = &out->rows[i];
        memset(r, 0, sizeof(*r));
        ray_t* s = ray_sym_vec_cell(tick, i); /* borrowed, do not release */
        if (!s) {
            op_logf(c, "extract: sym unresolvable at row %lld",
                    (long long)i);
            return false;
        }
        size_t len = ray_str_len(s);
        if (len >= STRESS_SYM_MAX) len = STRESS_SYM_MAX - 1;
        memcpy(r->ticker, ray_str_ptr(s), len);
        r->ticker[len] = '\0';
        r->price = pd[i];
        r->qty   = qd[i];
    }
    out->len = n;
    return true;
}

/* Persist a row array as a splayed dir, merging syms into the shared
 * symfile. */
static bool save_rows(stress_ctx_t* c, const char* dir,
                      const stress_rows_t* rows, bool bulk) {
    ray_t* tbl = build_table_from_rows(rows);
    if (!tbl) {
        op_logf(c, "save_rows: build failed for %s", dir);
        return false;
    }
    ray_err_t e = bulk ? ray_splay_save_bulk(tbl, dir, c->sym_path)
                       : ray_splay_save(tbl, dir, c->sym_path);
    ray_release(tbl);
    if (e != RAY_OK) {
        op_logf(c, "save_rows: save err=%d for %s", (int)e, dir);
        return false;
    }
    return true;
}

/* Load one splayed dir (heap or mmap) against the shared symfile. */
static ray_t* load_dir(stress_ctx_t* c, const char* dir, bool use_mmap) {
    return use_mmap ? ray_read_splayed(dir, c->sym_path)
                    : ray_splay_load(dir, c->sym_path);
}

/* ---- lifecycle ----------------------------------------------------------- */

bool stress_init(stress_ctx_t* c, const char* db_root, uint64_t seed) {
    memset(c, 0, sizeof(*c));
    snprintf(c->db_root, sizeof(c->db_root), "%s", db_root);
    snprintf(c->sym_path, sizeof(c->sym_path), "%s/.sym", db_root);
    c->seed = seed;
    c->rng  = seed | 1; /* xorshift state must be nonzero */
    c->oplog = (char(*)[128])malloc((size_t)STRESS_OPLOG_CAP * 128);
    if (!c->oplog) return false;
    rm_rf(c->db_root);
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", c->db_root);
    if (system(cmd) != 0) {
        free(c->oplog);
        c->oplog = NULL;
        return false;
    }
    op_logf(c, "init seed=%llu db=%s", (unsigned long long)seed, db_root);
    return true;
}

void stress_destroy(stress_ctx_t* c) {
    rows_free(&c->live);
    for (int i = 0; i < c->nparts; i++) rows_free(&c->parts[i]);
    free(c->oplog);
    c->oplog = NULL;
    if (!c->failed) rm_rf(c->db_root);
}

/* ---- initial seeding ------------------------------------------------------ */

bool stress_seed_initial(stress_ctx_t* c, int64_t live_rows, int nparts,
                         int64_t rows_per_part) {
    if (nparts > STRESS_MAX_PARTS) return false;
    op_logf(c, "seed live=%lld nparts=%d rpp=%lld", (long long)live_rows,
            nparts, (long long)rows_per_part);
    stress_row_t row;
    for (int64_t i = 0; i < live_rows; i++) {
        stress_gen_row(c, STRESS_SYMS_MIXED, &row);
        if (!stress_rows_append(&c->live, &row)) return false;
    }
    char dir[512];
    stress_live_dir(c, dir, sizeof(dir));
    if (!save_rows(c, dir, &c->live, false)) return false;
    for (int p = 0; p < nparts; p++) {
        /* Publish the partition BEFORE filling it so an early append/save
         * failure leaves parts[p] reachable for stress_destroy to free —
         * otherwise the rows appended below leak (nparts was bumped only
         * after the whole loop). */
        c->nparts = p + 1;
        snprintf(c->part_dates[p], sizeof(c->part_dates[p]), "2024.01.%02d",
                 p + 1);
        for (int64_t i = 0; i < rows_per_part; i++) {
            stress_gen_row(c, STRESS_SYMS_MIXED, &row);
            if (!stress_rows_append(&c->parts[p], &row)) return false;
        }
        stress_part_dir(c, p, dir, sizeof(dir));
        if (!save_rows(c, dir, &c->parts[p], false)) return false;
    }
    return true;
}

/* ---- destructive ops -------------------------------------------------------
 * Pattern for every op: load the CURRENT disk state, extract it to a plain
 * row array, apply the mutation to that array, rebuild + save.  The shadow
 * gets the same logical mutation applied independently.  Because the real
 * side starts from disk (not from the shadow), disk corruption propagates
 * and is caught at the next compare. */

/* Shared tail: extract dir -> mutate via cb -> save.  rows passed to cb is
 * the extracted DISK content. */
typedef bool (*mutate_cb_t)(stress_ctx_t* c, stress_rows_t* rows, void* arg);

static bool mutate_dir(stress_ctx_t* c, const char* dir, bool use_mmap,
                       bool bulk, mutate_cb_t cb, void* arg) {
    ray_t* tbl = load_dir(c, dir, use_mmap);
    if (!tbl || RAY_IS_ERR(tbl)) {
        op_logf(c, "mutate: load %s failed (%s)", dir,
                tbl ? ray_err_code(tbl) : "null");
        if (tbl) ray_release(tbl);
        return false;
    }
    stress_rows_t disk = {0};
    bool ok = extract_rows(c, tbl, &disk);
    ray_release(tbl);
    if (ok) ok = cb(c, &disk, arg);
    if (ok) ok = save_rows(c, dir, &disk, bulk);
    rows_free(&disk);
    return ok;
}

/* -- insert -- */

typedef struct {
    const stress_row_t* rows;
    int64_t             n;
} insert_arg_t;

static bool cb_insert(stress_ctx_t* c, stress_rows_t* disk, void* a) {
    (void)c;
    insert_arg_t* arg = (insert_arg_t*)a;
    for (int64_t i = 0; i < arg->n; i++)
        if (!stress_rows_append(disk, &arg->rows[i])) return false;
    return true;
}

bool stress_op_insert(stress_ctx_t* c, int64_t n, stress_sym_pattern_t pat,
                      bool bulk, bool via_mmap) {
    stress_row_t* fresh =
        (stress_row_t*)malloc((size_t)n * sizeof(stress_row_t));
    if (!fresh) return false;
    op_logf(c, "insert n=%lld pat=%d bulk=%d mmap=%d", (long long)n, (int)pat,
            (int)bulk, (int)via_mmap);
    for (int64_t i = 0; i < n; i++) stress_gen_row(c, pat, &fresh[i]);

    char dir[512];
    stress_live_dir(c, dir, sizeof(dir));
    insert_arg_t arg = { fresh, n };
    bool ok = mutate_dir(c, dir, via_mmap, bulk, cb_insert, &arg);
    if (ok)
        for (int64_t i = 0; i < n; i++)
            if (!stress_rows_append(&c->live, &fresh[i])) { ok = false; break; }
    free(fresh);
    return ok;
}

/* -- upsert: keyed on ticker; FIRST matching row updated, else append --
 * First-match is the LANGUAGE's upsert semantics (query.c breaks on the
 * first key hit); both drivers follow the language so the eval and C
 * executors cannot drift. */

/* Null-ticker rows all share the "" key, so an upsert with a null key
 * collates onto the first null row — intentional test semantics, applied
 * identically to disk and shadow. */
/* Returns index of first row whose ticker matches, or -1. */
int64_t stress_find_first_by_ticker(const stress_rows_t* rows,
                                    const char* ticker) {
    for (int64_t i = 0; i < rows->len; i++)
        if (strcmp(rows->rows[i].ticker, ticker) == 0) return i;
    return -1;
}

static bool cb_upsert(stress_ctx_t* c, stress_rows_t* disk, void* a) {
    (void)c;
    const stress_row_t* row = (const stress_row_t*)a;
    int64_t hit = stress_find_first_by_ticker(disk, row->ticker);
    if (hit >= 0) {
        disk->rows[hit] = *row;
        return true;
    }
    return stress_rows_append(disk, row);
}

bool stress_op_upsert(stress_ctx_t* c, stress_sym_pattern_t pat,
                      bool via_mmap) {
    stress_row_t row;
    stress_gen_row(c, pat, &row);
    op_logf(c, "upsert ticker=%s pat=%d mmap=%d", row.ticker, (int)pat,
            (int)via_mmap);
    char dir[512];
    stress_live_dir(c, dir, sizeof(dir));
    if (!mutate_dir(c, dir, via_mmap, false, cb_upsert, &row)) return false;
    int64_t hit = stress_find_first_by_ticker(&c->live, row.ticker);
    if (hit >= 0) {
        c->live.rows[hit] = row;
        return true;
    }
    return stress_rows_append(&c->live, &row);
}

/* -- trim: drop head/tail n rows of live (-1) or a partition -- */

typedef struct {
    bool    tail;
    int64_t n;
} trim_arg_t;

static bool cb_trim(stress_ctx_t* c, stress_rows_t* disk, void* a) {
    (void)c;
    trim_arg_t* t = (trim_arg_t*)a;
    stress_rows_trim(disk, t->tail, t->n);
    return true;
}

bool stress_op_trim(stress_ctx_t* c, int part_idx, bool tail, int64_t n) {
    op_logf(c, "trim part=%d tail=%d n=%lld", part_idx, (int)tail,
            (long long)n);
    char dir[512];
    stress_rows_t* shadow;
    if (part_idx < 0) {
        stress_live_dir(c, dir, sizeof(dir));
        shadow = &c->live;
    } else {
        if (part_idx >= c->nparts) return false;
        stress_part_dir(c, part_idx, dir, sizeof(dir));
        shadow = &c->parts[part_idx];
    }
    trim_arg_t t = { tail, n };
    if (!mutate_dir(c, dir, false, false, cb_trim, &t)) return false;
    stress_rows_trim(shadow, tail, n);
    return true;
}

/* -- parted append: add rows to an existing partition -- */

bool stress_op_part_append(stress_ctx_t* c, int part_idx, int64_t n,
                           stress_sym_pattern_t pat) {
    if (part_idx < 0 || part_idx >= c->nparts) return false;
    stress_row_t* fresh =
        (stress_row_t*)malloc((size_t)n * sizeof(stress_row_t));
    if (!fresh) return false;
    op_logf(c, "part_append part=%d n=%lld pat=%d", part_idx, (long long)n,
            (int)pat);
    for (int64_t i = 0; i < n; i++) stress_gen_row(c, pat, &fresh[i]);

    char dir[512];
    stress_part_dir(c, part_idx, dir, sizeof(dir));
    insert_arg_t arg = { fresh, n };
    bool ok = mutate_dir(c, dir, false, false, cb_insert, &arg);
    if (ok)
        for (int64_t i = 0; i < n; i++)
            if (!stress_rows_append(&c->parts[part_idx], &fresh[i])) {
                ok = false;
                break;
            }
    free(fresh);
    return ok;
}

/* -- new partition: next sequential date dir -- */

bool stress_op_part_new(stress_ctx_t* c, int64_t n, stress_sym_pattern_t pat) {
    if (c->nparts >= STRESS_MAX_PARTS) return true; /* fixture full: no-op */
    int p = c->nparts;
    snprintf(c->part_dates[p], sizeof(c->part_dates[p]), "2024.01.%02d",
             (p + 1) % 100); /* % keeps -Wformat-truncation provably in range */
    op_logf(c, "part_new date=%s n=%lld pat=%d", c->part_dates[p],
            (long long)n, (int)pat);
    stress_row_t row;
    for (int64_t i = 0; i < n; i++) {
        stress_gen_row(c, pat, &row);
        if (!stress_rows_append(&c->parts[p], &row)) {
            rows_free(&c->parts[p]);
            return false;
        }
    }
    char dir[512];
    stress_part_dir(c, p, dir, sizeof(dir));
    if (!save_rows(c, dir, &c->parts[p], false)) {
        rows_free(&c->parts[p]);
        return false;
    }
    c->nparts = p + 1;
    return true;
}

/* -- vocabulary growth: fresh tickers through the live table -- */

bool stress_op_sym_grow(stress_ctx_t* c, int64_t n) {
    op_logf(c, "sym_grow n=%lld", (long long)n);
    return stress_op_insert(c, n, STRESS_SYMS_NEW, false, false);
}

/* -- simulated process restart -------------------------------------------
 * Tear down the global sym table and reload it from the shared symfile,
 * exactly what a fresh process sees.  Every sym ID minted before this
 * call is void afterwards; the engine holds no ray_t across ops, and the
 * shadow stores strings, so only genuine save/merge bugs can surface. */

bool stress_op_restart(stress_ctx_t* c) {
    op_logf(c, "restart (sym reset + reload %s)", c->sym_path);
    ray_sym_destroy();
    ray_err_t e = ray_sym_init();
    if (e != RAY_OK) {
        op_logf(c, "restart: sym_init err=%d", (int)e);
        return false;
    }
    e = ray_sym_load(c->sym_path);
    if (e != RAY_OK) {
        stress_dump_failure(c, "restart: ray_sym_load err=%d — symfile unreadable "
                        "by a fresh process", (int)e);
        return false;
    }
    return true;
}

/* ---- verification ---------------------------------------------------------
 * Cell-by-cell compare of a loaded splayed dir against its shadow.  Tickers
 * are compared as strings via ray_sym_vec_lookup on the loaded column:
 * lookup(shadow) == disk_id is equivalent to cell-str(disk_id) == shadow
 * (a domain is a bijection), and catches enumeration shifts.  Resolving
 * through the COLUMN's domain keeps this oracle valid across the Phase-2
 * flip to per-vocabulary symfiles (exact no-op while every domain is the
 * runtime singleton).  NaN price compares equal to NaN (NULL_F64). */

static bool compare_dir(stress_ctx_t* c, const char* dir,
                        const stress_rows_t* shadow, bool use_mmap,
                        const char* label) {
    ray_t* tbl = load_dir(c, dir, use_mmap);
    if (!tbl || RAY_IS_ERR(tbl)) {
        stress_dump_failure(c, "%s: load failed (%s)", label,
                     tbl ? ray_err_code(tbl) : "null");
        if (tbl) ray_release(tbl);
        return false;
    }
    if (ray_table_ncols(tbl) != 3 || ray_table_nrows(tbl) != shadow->len) {
        stress_dump_failure(c, "%s: shape %lldx%lld, expected %lldx3", label,
                     (long long)ray_table_nrows(tbl),
                     (long long)ray_table_ncols(tbl), (long long)shadow->len);
        ray_release(tbl);
        return false;
    }
    ray_t* tick  = ray_table_get_col_idx(tbl, 0);
    ray_t* price = ray_table_get_col_idx(tbl, 1);
    ray_t* qty   = ray_table_get_col_idx(tbl, 2);
    if (!tick || !price || !qty || tick->len != shadow->len ||
        price->len != shadow->len || qty->len != shadow->len) {
        stress_dump_failure(c, "%s: column lengths disagree with row count", label);
        ray_release(tbl);
        return false;
    }
    const void*    td = ray_data(tick);
    const double*  pd = (const double*)ray_data(price);
    const int64_t* qd = (const int64_t*)ray_data(qty);
    for (int64_t i = 0; i < shadow->len; i++) {
        const stress_row_t* ex = &shadow->rows[i];
        int64_t disk_id = ray_read_sym(td, i, RAY_SYM, tick->attrs);
        int64_t want_id = ray_sym_vec_lookup(tick, ex->ticker, strlen(ex->ticker));
        if (disk_id != want_id) {
            ray_t* s = ray_sym_vec_cell(tick, i); /* borrowed; may be NULL */
            stress_dump_failure(c,
                "%s row %lld: ticker id %lld ('%.*s') != expected '%s' (id %lld)",
                label, (long long)i, (long long)disk_id,
                s ? (int)ray_str_len(s) : 1, s ? ray_str_ptr(s) : "?",
                ex->ticker, (long long)want_id);
            ray_release(tbl);
            return false;
        }
        bool price_eq = (pd[i] == ex->price) ||
                        (isnan(pd[i]) && isnan(ex->price));
        if (!price_eq) {
            stress_dump_failure(c, "%s row %lld: price %g != expected %g", label,
                         (long long)i, pd[i], ex->price);
            ray_release(tbl);
            return false;
        }
        if (qd[i] != ex->qty) {
            stress_dump_failure(c, "%s row %lld: qty %lld != expected %lld", label,
                         (long long)i, (long long)qd[i], (long long)ex->qty);
            ray_release(tbl);
            return false;
        }
    }
    ray_release(tbl);
    return true;
}

/* Structural invariants on the shared symfile (raw file read, no API):
 * magic, and count monotonically non-decreasing across the run. */
bool stress_check_invariants(stress_ctx_t* c) {
    FILE* f = fopen(c->sym_path, "rb");
    if (!f) {
        stress_dump_failure(c, "invariant: symfile %s missing", c->sym_path);
        return false;
    }
    uint32_t magic = 0;
    int64_t  cnt   = -1;
    size_t ok = fread(&magic, sizeof(magic), 1, f);
    ok += fread(&cnt, sizeof(cnt), 1, f);
    fclose(f);
    if (ok != 2) {
        stress_dump_failure(c, "invariant: symfile header truncated (read %zu/2 fields)",
                     ok);
        return false;
    }
    if (magic != 0x4C525453u) { /* "STRL" */
        stress_dump_failure(c, "invariant: symfile bad magic 0x%x", magic);
        return false;
    }
    if (cnt < c->last_sym_count) {
        stress_dump_failure(c, "invariant: symfile count shrank %lld -> %lld",
                     (long long)c->last_sym_count, (long long)cnt);
        return false;
    }
    /* Post-flip domain invariants: the symfile holds the tables'
     * VOCABULARY (no relation to the global table's count anymore — the
     * old `cnt <= ray_sym_count()` bound is gone with the global dump).
     * Position 0 must be the reserved empty string, and the vocabulary
     * can never exceed every distinct symbol this run generated plus ""
     * (sym_uniq fresh names + the null) — a gross upper bound that
     * catches runaway duplication. */
    if (cnt > 0) {
        FILE* f2 = fopen(c->sym_path, "rb");
        uint32_t len0 = 1;
        bool ok = f2 && fseek(f2, 12, SEEK_SET) == 0 &&
                  fread(&len0, sizeof(len0), 1, f2) == 1;
        if (f2) fclose(f2);
        if (!ok || len0 != 0) {
            stress_dump_failure(c, "invariant: symfile position 0 is not \"\" "
                            "(len=%u ok=%d)", len0, (int)ok);
            return false;
        }
    }
    if (cnt > (int64_t)c->sym_uniq + 1) {
        stress_dump_failure(c, "invariant: symfile count %lld > distinct symbols "
                        "ever generated %d + null", (long long)cnt,
                     c->sym_uniq);
        return false;
    }
    c->last_sym_count = cnt;
    return true;
}

bool stress_verify_all(stress_ctx_t* c, bool use_mmap) {
    op_logf(c, "verify mmap=%d", (int)use_mmap);
    if (!stress_check_invariants(c)) return false;
    char dir[512];
    stress_live_dir(c, dir, sizeof(dir));
    if (!compare_dir(c, dir, &c->live, use_mmap, "live")) return false;
    for (int p = 0; p < c->nparts; p++) {
        stress_part_dir(c, p, dir, sizeof(dir));
        char label[32];
        snprintf(label, sizeof(label), "part[%s]", c->part_dates[p]);
        if (!compare_dir(c, dir, &c->parts[p], use_mmap, label)) return false;
    }
    /* the parted loader itself must accept the mixed-layout root */
    if (c->nparts > 0) {
        ray_t* parted = ray_read_parted(c->db_root, "hist");
        if (!parted || RAY_IS_ERR(parted)) {
            stress_dump_failure(c, "ray_read_parted failed (%s)",
                         parted ? ray_err_code(parted) : "null");
            if (parted) ray_release(parted);
            return false;
        }
        ray_release(parted);
    }
    return true;
}
