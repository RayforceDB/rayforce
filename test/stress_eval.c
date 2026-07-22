/*
 * stress_eval.c — Rayfall eval driver for the stress engine
 * (stress_store.c).  One engine, two drivers: each eval op builds Rayfall
 * source from the SAME stress_gen_row output that feeds the shadow, runs
 * it via ray_eval_str, and applies the identical logical mutation to the
 * shadow.  The phase-1 C verifier (stress_verify_all) is shared.
 *
 * Verified syntax sources of truth (never guessed):
 *   - null literals (test/rfl/null/ + REPL probe): empty symbol = `'`
 *     (SYM columns are no-null by design, sym 0 = "" renders as ');
 *     f64 null = `0Nf`; i64 null = `0Nl`.  All three are accepted inside
 *     plain vector literals, including in leading position.
 *   - the .db trio (test/rfl/system/db_get.rfl, db_sym_resolution.rfl):
 *     `(.db.splayed.set "<dir>" <table> "<root>/.sym")` — the EXPLICIT
 *     third argument is required for the shared root symfile (no
 *     root-walk for plain dirs post-flip).
 */

#include "stress_eval.h"
#include <rayforce.h>
#include "table/sym.h" /* ray_sym_vec_cell: group keys may carry FILE domains */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- runtime lifecycle ---------------------------------------------------
 * ray_runtime_create() itself runs ray_heap_init() + ray_sym_init(), and
 * ray_runtime_destroy() runs ray_sym_destroy() + ray_heap_destroy()
 * (src/core/runtime.c) — so suites pair runtime_up/runtime_down ALONE,
 * with no separate heap/sym setup (mirrors test_domain.c's
 * domain_rt_setup/teardown). */

static ray_runtime_t* g_rt;

bool stress_eval_runtime_up(void) {
    if (g_rt) return true;
    g_rt = ray_runtime_create(0, NULL);
    return g_rt != NULL;
}

void stress_eval_runtime_down(void) {
    if (!g_rt) return;
    ray_runtime_destroy(g_rt);
    g_rt = NULL;
}

/* ---- eval ----------------------------------------------------------------- */

/* Log every source line first (op_logf truncates to the 128-byte log
 * line) so any failure is replayable by hand in the REPL. */
static void log_src_lines(stress_ctx_t* c, const char* src) {
    for (const char* p = src; *p;) {
        const char* nl  = strchr(p, '\n');
        size_t      len = nl ? (size_t)(nl - p) : strlen(p);
        stress_op_logf(c, "eval %.*s", (int)len, p);
        p += len + (nl ? 1 : 0);
    }
}

bool stress_eval_exec(stress_ctx_t* c, const char* src) {
    log_src_lines(c, src);
    if (!g_rt) {
        stress_dump_failure(c, "eval: no runtime (stress_eval_runtime_up "
                               "not called?)");
        return false;
    }
    /* Successful void/null evaluation returns RAY_NULL_OBJ; bare NULL remains
     * a defensive no-result fallback in this stress harness. */
    ray_t* r = ray_eval_str(src);
    if (r && RAY_IS_ERR(r)) {
        stress_dump_failure(c, "eval error '%s' for: %.300s",
                            ray_err_code(r), src);
        ray_release(r);
        return false;
    }
    if (r) ray_release(r);
    return true;
}

/* ---- float-exactness discipline -------------------------------------------
 * The shadow must hold EXACTLY the double the language will produce from
 * the generated source, or the verifier's bit-for-bit price compare (and
 * later the query oracle's sums) diverge in ULPs.  Discipline: prices are
 * emitted with one fixed format (PRICE_FMT), and a generated row's shadow
 * price is strtod() OF THAT EMITTED TEXT (canon_row_price below, applied
 * to every generated row before it reaches the shadow).  The table builder
 * re-emits PRICE_FMT from the stored double; format→parse→format is a
 * fixed point over the generated range (0.00..9999.99: the parsed double
 * is within half an ulp of the decimal value, far closer than half a
 * hundredth), so source text and shadow agree bit-for-bit. */

#define PRICE_FMT "%.2f"

static void canon_row_price(stress_row_t* r) {
    if (isnan(r->price)) return;  /* null price stays NULL_F64 */
    char txt[64];
    snprintf(txt, sizeof(txt), PRICE_FMT, r->price);
    r->price = strtod(txt, NULL);
}

/* ---- table-literal builder ------------------------------------------------ */

/* snprintf-append into buf at *off; false if it would not fit. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 4, 5)))
#endif
static bool emit(char* buf, size_t bufsz, size_t* off, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(buf + *off, bufsz - *off, fmt, ap);
    va_end(ap);
    if (w < 0 || (size_t)w >= bufsz - *off) return false;
    *off += (size_t)w;
    return true;
}

bool stress_eval_table_src(const stress_rows_t* rows, int64_t from, int64_t n,
                           char* buf, size_t bufsz) {
    if (from < 0 || n < 0 || from + n > rows->len) return false;
    size_t off = 0;
    if (!emit(buf, bufsz, &off, "(table [ticker price qty] (list ")) return false;
    /* empty slice: typed empty columns (a bare [] is I64) */
    if (n == 0)
        return emit(buf, bufsz, &off,
                    "(as 'SYM []) (as 'F64 []) []))");
    /* ticker column: 'sym literals; empty ticker ("") = ' (sym 0) */
    if (!emit(buf, bufsz, &off, "[")) return false;
    for (int64_t i = 0; i < n; i++) {
        const stress_row_t* r = &rows->rows[from + i];
        const char* sp = i ? " " : "";
        if (r->ticker[0] == '\0') {
            if (!emit(buf, bufsz, &off, "%s'", sp)) return false;
        } else {
            if (!emit(buf, bufsz, &off, "%s'%s", sp, r->ticker)) return false;
        }
    }
    /* price column: PRICE_FMT (see discipline above); null = 0Nf */
    if (!emit(buf, bufsz, &off, "] [")) return false;
    for (int64_t i = 0; i < n; i++) {
        const stress_row_t* r = &rows->rows[from + i];
        const char* sp = i ? " " : "";
        if (isnan(r->price)) {
            if (!emit(buf, bufsz, &off, "%s0Nf", sp)) return false;
        } else {
            if (!emit(buf, bufsz, &off, "%s" PRICE_FMT, sp, r->price))
                return false;
        }
    }
    /* qty column; null = 0Nl */
    if (!emit(buf, bufsz, &off, "] [")) return false;
    for (int64_t i = 0; i < n; i++) {
        const stress_row_t* r = &rows->rows[from + i];
        const char* sp = i ? " " : "";
        if (r->qty == NULL_I64) {
            if (!emit(buf, bufsz, &off, "%s0Nl", sp)) return false;
        } else {
            if (!emit(buf, bufsz, &off, "%s%lld", sp, (long long)r->qty))
                return false;
        }
    }
    return emit(buf, bufsz, &off, "]))");
}

/* Conservative source-buffer size for a slice of n rows wrapped in one
 * surrounding get→mutate→set form (three quoted dirs + three quoted sym
 * paths ≤ 6*512, plus verb syntax). */
static size_t src_bufsz(int64_t n) {
    /* per row: ticker (≤31+2) + price (≤8) + qty (≤21) + separators */
    return 4096 + (size_t)n * (3 * STRESS_SYM_MAX + 16);
}

/* (.db.splayed.set "<dir>" <table literal> "<shared sym>") — the explicit
 * third argument shares the root symfile (client layout; see header
 * comment). */
static bool eval_set_dir(stress_ctx_t* c, const char* dir,
                         const stress_rows_t* rows) {
    size_t cap = src_bufsz(rows->len);
    char*  src = (char*)malloc(cap);
    if (!src) return false;
    size_t off = 0;
    bool ok = emit(src, cap, &off, "(.db.splayed.set \"%s\" ", dir);
    if (ok) {
        ok = stress_eval_table_src(rows, 0, rows->len, src + off, cap - off);
        off += strlen(src + off);
    }
    if (ok) ok = emit(src, cap, &off, " \"%s\")", c->sym_path);
    if (!ok)
        stress_op_logf(c, "eval_set_dir: source buffer too small for %s", dir);
    if (ok) ok = stress_eval_exec(c, src);
    free(src);
    return ok;
}

/* ---- initial seeding -------------------------------------------------------
 * Shadow exactly like the engine's stress_seed_initial (same patterns, same
 * rng consumption, same dates); real side through generated Rayfall. */

bool stress_eval_seed_initial(stress_ctx_t* c, int64_t live_rows, int nparts,
                              int64_t rows_per_part) {
    if (nparts > STRESS_MAX_PARTS) return false;
    stress_op_logf(c, "eval-seed live=%lld nparts=%d rpp=%lld",
                   (long long)live_rows, nparts, (long long)rows_per_part);
    stress_row_t row;
    for (int64_t i = 0; i < live_rows; i++) {
        stress_gen_row(c, STRESS_SYMS_MIXED, &row);
        canon_row_price(&row);
        if (!stress_rows_append(&c->live, &row)) return false;
    }
    char dir[512];
    stress_live_dir(c, dir, sizeof(dir));
    if (!eval_set_dir(c, dir, &c->live)) return false;
    for (int p = 0; p < nparts; p++) {
        /* Publish before filling so an early failure frees parts[p] (see
         * stress_seed_initial for the rationale). */
        c->nparts = p + 1;
        snprintf(c->part_dates[p], sizeof(c->part_dates[p]), "2024.01.%02d",
                 (p + 1) % 100); /* % keeps -Wformat-truncation in range */
        for (int64_t i = 0; i < rows_per_part; i++) {
            stress_gen_row(c, STRESS_SYMS_MIXED, &row);
            canon_row_price(&row);
            if (!stress_rows_append(&c->parts[p], &row)) return false;
        }
        stress_part_dir(c, p, dir, sizeof(dir));
        if (!eval_set_dir(c, dir, &c->parts[p])) return false;
    }
    return true;
}

/* ---- op executors -----------------------------------------------------------
 * One engine, two drivers: each executor consumes the ctx rng EXACTLY like
 * its C counterpart (stress_store.c) and applies the identical shadow
 * mutation, so the same seed yields bit-identical shadows under either
 * driver (pinned by stress_eval/equiv_with_c_driver).  The real side is a
 * SINGLE eval string — get → mutate → set — relying on sequential
 * evaluation of multiple top-level forms; a failure therefore replays as
 * one REPL paste from the op log.
 *
 * Verified verb forms (test/rfl/table/update.rfl, rfl/null/upsert.rfl,
 * src/ops/collection.c ray_take_fn):
 *   (insert 'T <table literal>)   quoted sym = in-place env update;
 *                                 table payload matches by column name
 *   (upsert 'T 1 <row list>)      1 = leading key-column count (ticker);
 *                                 FIRST matching row updated, else append
 *   (take T n) / (take T -n)      first / last n rows; n ≤ len here, so
 *                                 take's wrap-extension never triggers
 */

/* Append rows to the splayed table at dir via (insert 'ST ...). */
static bool eval_insert_rows(stress_ctx_t* c, const char* dir,
                             const stress_row_t* fresh, int64_t n) {
    const stress_rows_t view = { (stress_row_t*)fresh, n, n };
    size_t cap = src_bufsz(n);
    char*  src = (char*)malloc(cap);
    if (!src) return false;
    size_t off = 0;
    bool ok = emit(src, cap, &off,
                   "(set ST (.db.splayed.get \"%s\" \"%s\"))\n(insert 'ST ",
                   dir, c->sym_path);
    if (ok) {
        ok = stress_eval_table_src(&view, 0, n, src + off, cap - off);
        off += strlen(src + off);
    }
    if (ok)
        ok = emit(src, cap, &off, ")\n(.db.splayed.set \"%s\" ST \"%s\")",
                  dir, c->sym_path);
    if (!ok)
        stress_op_logf(c, "eval insert: source buffer too small for %s", dir);
    if (ok) ok = stress_eval_exec(c, src);
    free(src);
    return ok;
}

bool stress_eval_op_insert(stress_ctx_t* c, int64_t n,
                           stress_sym_pattern_t pat) {
    stress_row_t* fresh =
        (stress_row_t*)malloc((size_t)n * sizeof(stress_row_t));
    if (!fresh) return false;
    stress_op_logf(c, "eval-insert n=%lld pat=%d", (long long)n, (int)pat);
    for (int64_t i = 0; i < n; i++) {
        stress_gen_row(c, pat, &fresh[i]);
        canon_row_price(&fresh[i]);
    }
    char dir[512];
    stress_live_dir(c, dir, sizeof(dir));
    bool ok = eval_insert_rows(c, dir, fresh, n);
    if (ok)
        for (int64_t i = 0; i < n; i++)
            if (!stress_rows_append(&c->live, &fresh[i])) { ok = false; break; }
    free(fresh);
    return ok;
}

/* One row's values as the upsert payload: (list 'tkr 12.34 56), with the
 * verified literals (empty sym ' / 0Nf / 0Nl) per cell. */
static bool emit_row_list(char* buf, size_t cap, size_t* off,
                          const stress_row_t* r) {
    bool ok = r->ticker[0] == '\0'
                  ? emit(buf, cap, off, "(list ' ")
                  : emit(buf, cap, off, "(list '%s ", r->ticker);
    if (ok)
        ok = isnan(r->price) ? emit(buf, cap, off, "0Nf ")
                             : emit(buf, cap, off, PRICE_FMT " ", r->price);
    if (ok)
        ok = r->qty == NULL_I64
                 ? emit(buf, cap, off, "0Nl)")
                 : emit(buf, cap, off, "%lld)", (long long)r->qty);
    return ok;
}

bool stress_eval_op_upsert(stress_ctx_t* c, stress_sym_pattern_t pat) {
    stress_row_t row;
    stress_gen_row(c, pat, &row);
    canon_row_price(&row);
    stress_op_logf(c, "eval-upsert ticker=%s pat=%d", row.ticker, (int)pat);
    char dir[512];
    stress_live_dir(c, dir, sizeof(dir));
    char   src[4096];
    size_t off = 0;
    bool ok = emit(src, sizeof(src), &off,
                   "(set ST (.db.splayed.get \"%s\" \"%s\"))\n(upsert 'ST 1 ",
                   dir, c->sym_path);
    if (ok) ok = emit_row_list(src, sizeof(src), &off, &row);
    if (ok)
        ok = emit(src, sizeof(src), &off,
                  ")\n(.db.splayed.set \"%s\" ST \"%s\")", dir, c->sym_path);
    if (ok) ok = stress_eval_exec(c, src);
    if (!ok) return false;
    /* shadow: language semantics — FIRST matching key updated, else append */
    int64_t hit = stress_find_first_by_ticker(&c->live, row.ticker);
    if (hit >= 0) {
        c->live.rows[hit] = row;
        return true;
    }
    return stress_rows_append(&c->live, &row);
}

bool stress_eval_op_trim(stress_ctx_t* c, int part_idx, bool tail, int64_t n) {
    stress_op_logf(c, "eval-trim part=%d tail=%d n=%lld", part_idx, (int)tail,
                   (long long)n);
    char           dir[512];
    stress_rows_t* shadow;
    if (part_idx < 0) {
        stress_live_dir(c, dir, sizeof(dir));
        shadow = &c->live;
    } else {
        if (part_idx >= c->nparts) return false;
        stress_part_dir(c, part_idx, dir, sizeof(dir));
        shadow = &c->parts[part_idx];
    }
    /* No `drop` verb: trim = keep the complement via take.  drop-tail n
     * keeps the FIRST len-n rows → (take T keep); drop-head n keeps the
     * LAST len-n rows → (take T -keep).  Clamp like stress_rows_trim, so
     * keep ∈ [0, len] and take never wrap-extends. */
    int64_t len  = shadow->len;
    int64_t drop = n < 0 ? 0 : (n > len ? len : n);
    long long take_arg = (long long)(tail ? len - drop : -(len - drop));
    char   src[4096];
    size_t off = 0;
    bool ok = emit(src, sizeof(src), &off,
                   "(set ST (.db.splayed.get \"%s\" \"%s\"))\n"
                   "(set ST (take ST %lld))\n"
                   "(.db.splayed.set \"%s\" ST \"%s\")",
                   dir, c->sym_path, take_arg, dir, c->sym_path);
    if (ok) ok = stress_eval_exec(c, src);
    if (!ok) return false;
    stress_rows_trim(shadow, tail, n);
    return true;
}

bool stress_eval_op_part_append(stress_ctx_t* c, int part_idx, int64_t n,
                                stress_sym_pattern_t pat) {
    if (part_idx < 0 || part_idx >= c->nparts) return false;
    stress_row_t* fresh =
        (stress_row_t*)malloc((size_t)n * sizeof(stress_row_t));
    if (!fresh) return false;
    stress_op_logf(c, "eval-part_append part=%d n=%lld pat=%d", part_idx,
                   (long long)n, (int)pat);
    for (int64_t i = 0; i < n; i++) {
        stress_gen_row(c, pat, &fresh[i]);
        canon_row_price(&fresh[i]);
    }
    char dir[512];
    stress_part_dir(c, part_idx, dir, sizeof(dir));
    bool ok = eval_insert_rows(c, dir, fresh, n);
    if (ok)
        for (int64_t i = 0; i < n; i++)
            if (!stress_rows_append(&c->parts[part_idx], &fresh[i])) {
                ok = false;
                break;
            }
    free(fresh);
    return ok;
}

bool stress_eval_op_part_new(stress_ctx_t* c, int64_t n,
                             stress_sym_pattern_t pat) {
    if (c->nparts >= STRESS_MAX_PARTS) return true; /* fixture full: no-op */
    int p = c->nparts;
    snprintf(c->part_dates[p], sizeof(c->part_dates[p]), "2024.01.%02d",
             (p + 1) % 100); /* % keeps -Wformat-truncation in range */
    stress_op_logf(c, "eval-part_new date=%s n=%lld pat=%d", c->part_dates[p],
                   (long long)n, (int)pat);
    stress_row_t row;
    for (int64_t i = 0; i < n; i++) {
        stress_gen_row(c, pat, &row);
        canon_row_price(&row);
        if (!stress_rows_append(&c->parts[p], &row)) goto fail;
    }
    char dir[512];
    stress_part_dir(c, p, dir, sizeof(dir));
    if (!eval_set_dir(c, dir, &c->parts[p])) goto fail;
    c->nparts = p + 1;
    return true;
fail:
    free(c->parts[p].rows);
    memset(&c->parts[p], 0, sizeof(c->parts[p]));
    return false;
}

/* Simulated process restart, eval flavor: tear the WHOLE runtime down and
 * recreate it (heap + sym + env all die — every binding and sym id minted
 * before this call is void).  Nothing is reloaded here: the next op's
 * get re-opens its table against the shared symfile, exactly like a fresh
 * process would. */
bool stress_eval_op_restart(stress_ctx_t* c) {
    stress_op_logf(c, "eval-restart (runtime destroy + create)");
    stress_eval_runtime_down();
    if (!stress_eval_runtime_up()) {
        stress_dump_failure(c, "eval-restart: ray_runtime_create failed");
        return false;
    }
    return true;
}

/* ---- query oracle -----------------------------------------------------------
 * Shadow-computed expectations vs the SAME queries evaluated by the
 * language over the persisted fixture.  The shadow mirrors the LANGUAGE's
 * null semantics, all REPL-probed (never guessed):
 *
 *   (sum v)    skips null sentinels; empty / all-null sum = 0 (i64), NOT null
 *   (count v)  = vector length — COUNT(*) semantics, null cells counted
 *   group-by   buckets the empty sym ("" = ' = sym 0) as its OWN group
 *   inner-join matches null sym keys like any other key
 *
 * Aggregates are over QTY (int64) ONLY — exact under any summation order.
 * Price (f64) is deliberately NOT summed: double addition is
 * order-dependent and the language's group/scan order is not the shadow's
 * row order, so a price-sum oracle would need an epsilon that could mask
 * real off-by-one-row bugs (disclosed decision; per-cell price equality is
 * already pinned bit-for-bit by stress_verify_all).
 *
 * Every query eval is SELF-CONTAINED (starts with its own .db get), so a
 * failing query replays as one REPL paste from the op log even right after
 * a restart. */

/* Eval one query; returns an owned non-error result, or NULL after dumping
 * the failure (a query never legitimately returns void/null here). */
static ray_t* eval_query(stress_ctx_t* c, const char* src) {
    log_src_lines(c, src);
    if (!g_rt) {
        stress_dump_failure(c, "query: no runtime");
        return NULL;
    }
    ray_t* r = ray_eval_str(src);
    if (!r || RAY_IS_NULL(r)) {
        stress_dump_failure(c, "query: void/null result for: %.300s", src);
        return NULL;
    }
    if (RAY_IS_ERR(r)) {
        stress_dump_failure(c, "query: error '%s' for: %.300s",
                            ray_err_code(r), src);
        ray_release(r);
        return NULL;
    }
    return r;
}

/* Eval a query whose result must be an I64 atom (count/sum results). */
static bool eval_query_i64(stress_ctx_t* c, const char* src, int64_t* out) {
    ray_t* r = eval_query(c, src);
    if (!r) return false;
    if (r->type != -RAY_I64) {
        stress_dump_failure(c, "query: expected i64 atom, got type %d for: "
                               "%.300s", (int)r->type, src);
        ray_release(r);
        return false;
    }
    *out = r->i64;
    ray_release(r);
    return true;
}

/* Shadow scans (the pool is <=256 entries and fixtures are small; O(n^2)
 * array scans are deliberate — no hash map to get subtly wrong). */

static int64_t rows_key_count(const stress_rows_t* r, const char* key) {
    int64_t n = 0;
    for (int64_t i = 0; i < r->len; i++)
        if (strcmp(r->rows[i].ticker, key) == 0) n++;
    return n;
}

static int64_t rows_distinct_tickers(const stress_rows_t* r) {
    int64_t d = 0;
    for (int64_t i = 0; i < r->len; i++) {
        bool seen = false;
        for (int64_t j = 0; j < i && !seen; j++)
            seen = strcmp(r->rows[i].ticker, r->rows[j].ticker) == 0;
        if (!seen) d++;
    }
    return d;
}

/* hist = union of the partition shadows */
static int64_t hist_key_count(const stress_ctx_t* c, const char* key) {
    int64_t n = 0;
    for (int p = 0; p < c->nparts; p++)
        n += rows_key_count(&c->parts[p], key);
    return n;
}

/* (a) filtered aggregate: one pooled ticker, language-side
 * count(qty)/sum(qty) over `where ticker == k` vs the shadow. */
static bool verify_filtered_agg(stress_ctx_t* c, const char* live_dir) {
    if (c->pool_len == 0) {
        stress_op_logf(c, "query-oracle (a) skipped: ticker pool empty");
        return true;
    }
    int span = c->pool_len < STRESS_POOL_CAP ? c->pool_len : STRESS_POOL_CAP;
    const char* tkr = c->pool[stress_rand(c) % (uint64_t)span];
    int64_t exp_cnt = 0, exp_sum = 0;
    for (int64_t i = 0; i < c->live.len; i++) {
        if (strcmp(c->live.rows[i].ticker, tkr) != 0) continue;
        exp_cnt++;                           /* count = COUNT(*), nulls in */
        if (c->live.rows[i].qty != NULL_I64) /* sum skips nulls; empty = 0 */
            exp_sum += c->live.rows[i].qty;
    }
    stress_op_logf(c, "query-oracle (a) ticker=%s expect cnt=%lld sum=%lld",
                   tkr, (long long)exp_cnt, (long long)exp_sum);
    char    src[2048];
    int64_t got;
    snprintf(src, sizeof(src),
             "(set QT (.db.splayed.get \"%s\" \"%s\"))\n"
             "(count (at (select {from: QT where: (== ticker '%s)}) 'qty))",
             live_dir, c->sym_path, tkr);
    if (!eval_query_i64(c, src, &got)) return false;
    if (got != exp_cnt) {
        stress_dump_failure(c, "query (a) count: got %lld, shadow %lld for "
                               "ticker '%s'",
                            (long long)got, (long long)exp_cnt, tkr);
        return false;
    }
    snprintf(src, sizeof(src),
             "(set QT (.db.splayed.get \"%s\" \"%s\"))\n"
             "(sum (at (select {from: QT where: (== ticker '%s)}) 'qty))",
             live_dir, c->sym_path, tkr);
    if (!eval_query_i64(c, src, &got)) return false;
    if (got != exp_sum) {
        stress_dump_failure(c, "query (a) sum(qty): got %lld, shadow %lld "
                               "for ticker '%s'",
                            (long long)got, (long long)exp_sum, tkr);
        return false;
    }
    return true;
}

/* (b) group-by counts: every returned (key, count) pair vs the shadow,
 * order-independent (per-row shadow lookup), plus total group count vs
 * shadow distinct tickers (the "" null group counts like any other). */
static bool verify_group_counts(stress_ctx_t* c, const char* live_dir) {
    char src[2048];
    snprintf(src, sizeof(src),
             "(set QT (.db.splayed.get \"%s\" \"%s\"))\n"
             "(select {from: QT by: ticker c: (count qty)})",
             live_dir, c->sym_path);
    ray_t* g = eval_query(c, src);
    if (!g) return false;
    bool ok = false;
    if (g->type != RAY_TABLE) {
        stress_dump_failure(c, "query (b): expected table, got type %d",
                            (int)g->type);
        goto done;
    }
    {
        int64_t ngroups   = ray_table_nrows(g);
        int64_t edistinct = rows_distinct_tickers(&c->live);
        if (ngroups != edistinct) {
            stress_dump_failure(c, "query (b): %lld groups, shadow has %lld "
                                   "distinct tickers",
                                (long long)ngroups, (long long)edistinct);
            goto done;
        }
        ray_t* kcol = ray_table_get_col(g, ray_sym_intern("ticker", 6));
        ray_t* ccol = ray_table_get_col(g, ray_sym_intern("c", 1));
        if (!kcol || !ccol) {
            stress_dump_failure(c, "query (b): result lacks ticker/c column");
            goto done;
        }
        for (int64_t i = 0; i < ngroups; i++) {
            /* group keys may resolve through a FILE domain — never raw ids */
            ray_t* s = ray_sym_vec_cell(kcol, i);
            if (!s) {
                stress_dump_failure(c, "query (b): unresolvable group key at "
                                       "row %lld", (long long)i);
                goto done;
            }
            const char* kp = ray_str_ptr(s);
            size_t      kl = ray_str_len(s);
            char        key[STRESS_SYM_MAX];
            if (kl >= sizeof(key)) {
                stress_dump_failure(c, "query (b): group key %.40s... longer "
                                       "than any generated ticker", kp);
                goto done;
            }
            memcpy(key, kp, kl);
            key[kl] = '\0';
            int64_t want = rows_key_count(&c->live, key);
            int64_t got  = ray_vec_get_i64(ccol, i);
            /* want == 0 (phantom group) also lands here: got >= 1 always */
            if (got != want) {
                stress_dump_failure(c, "query (b): group '%s' count %lld, "
                                       "shadow %lld",
                                    key, (long long)got, (long long)want);
                goto done;
            }
        }
        /* counts summing to live.len is implied: groups are exactly the
         * shadow's distinct keys and each count matched the shadow scan */
        ok = true;
    }
done:
    ray_release(g);
    return ok;
}

/* (c) join cardinality: count of the language's inner-join live x hist on
 * ticker vs the shadow's sum over distinct live tickers of
 * live_count(k) * hist_count(k) — including the "" null key, which the
 * join matches like any other (probed).  Sides are projected to disjoint
 * non-key column names first (live/hist share price+qty names). */
static bool verify_join_cardinality(stress_ctx_t* c, const char* live_dir) {
    if (c->nparts == 0) {
        stress_op_logf(c, "query-oracle (c) skipped: no hist partitions");
        return true;
    }
    int64_t expect = 0;
    for (int64_t i = 0; i < c->live.len; i++) {
        const char* k = c->live.rows[i].ticker;
        bool seen = false;
        for (int64_t j = 0; j < i && !seen; j++)
            seen = strcmp(k, c->live.rows[j].ticker) == 0;
        if (seen) continue; /* count each distinct live ticker once */
        expect += rows_key_count(&c->live, k) * hist_key_count(c, k);
    }
    stress_op_logf(c, "query-oracle (c) expect |live ij hist| = %lld",
                   (long long)expect);
    char src[2048];
    snprintf(src, sizeof(src),
             "(set QT (.db.splayed.get \"%s\" \"%s\"))\n"
             "(set HT (.db.parted.get \"%s/\" 'hist))\n"
             "(set LJ (select {ticker: ticker lq: qty from: QT}))\n"
             "(set RJ (select {ticker: ticker rq: qty from: HT}))\n"
             "(count (inner-join [ticker] LJ RJ))",
             live_dir, c->sym_path, c->db_root);
    int64_t got;
    if (!eval_query_i64(c, src, &got)) return false;
    if (got != expect) {
        int64_t hist_len = 0;
        for (int p = 0; p < c->nparts; p++) hist_len += c->parts[p].len;
        stress_dump_failure(c, "query (c): inner-join cardinality %lld, "
                               "shadow expects %lld [live=%lld hist=%lld "
                               "nparts=%d live\"\"=%lld hist\"\"=%lld]",
                            (long long)got, (long long)expect,
                            (long long)c->live.len, (long long)hist_len,
                            c->nparts, (long long)rows_key_count(&c->live, ""),
                            (long long)hist_key_count(c, ""));
        return false;
    }
    return true;
}

bool stress_eval_verify_queries(stress_ctx_t* c) {
    char live_dir[512];
    stress_live_dir(c, live_dir, sizeof(live_dir));
    return verify_filtered_agg(c, live_dir) &&
           verify_group_counts(c, live_dir) &&
           verify_join_cardinality(c, live_dir);
}
