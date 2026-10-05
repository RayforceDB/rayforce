/* Compressed-columns P0 estimator.
 *
 * Measures, for a splayed table or a parted database, the on-disk size
 * breakdown (column data, inline indexes, symfile, other), the size of the
 * column data under the generation-2 integer encodings (per-vector choice
 * per the spec's selection rule, including null bitmaps, descriptors,
 * chunk directory, headers and VALUE_DICT side data), the encoding mix,
 * the chunk/vector skip rates of a predicate list, and an FSST estimate of
 * the symfile with its decode time and decompressed-snapshot size.
 *
 * Build: make lib && cc -O2 -std=c17 -Wall -Wextra -Iinclude -Isrc \
 *          bench/compress_estimate/main.c librayforce.a -lm -lpthread \
 *          -o build/compress_estimate
 */
#define _POSIX_C_SOURCE 200809L
#include <rayforce.h>
#include "mem/heap.h"
#include "table/sym.h"
#include "table/domain.h"
#include "store/splay.h"
#include "store/enc.h"
#include "store/fsst.h"
#include "lang/format.h"     /* ray_type_name */
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>

#define MAX_PRED 256

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec * 1e-9; }
static int64_t fsize(const char* p) { struct stat st; return stat(p, &st) == 0 ? (int64_t)st.st_size : 0; }
static void* xmalloc(size_t n) { void* p = malloc(n ? n : 1); if (!p) { fprintf(stderr, "out of memory\n"); exit(2); } return p; }
static void* xcalloc(size_t n, size_t m) { void* p = calloc(n ? n : 1, m ? m : 1); if (!p) { fprintf(stderr, "out of memory\n"); exit(2); } return p; }

/* ---- predicates ---- */
typedef struct { char group[32], col[64], op[4]; int nv; int64_t v[8]; bool sym_empty; } pred_t;
static pred_t g_pred[MAX_PRED]; static int g_npred;

static int64_t parse_const(const char* s, bool* sym_empty) {
    *sym_empty = strcmp(s, "\"\"") == 0;
    if (*sym_empty) return 0;
    int y, mo, d;
    if (sscanf(s, "%d.%d.%d", &y, &mo, &d) == 3) {
        /* days since 2000.01.01 (civil-from-days, proleptic Gregorian) */
        int yy = y - (mo <= 2); int era = (yy >= 0 ? yy : yy - 399) / 400;
        unsigned yoe = (unsigned)(yy - era * 400);
        unsigned doy = (153u * (unsigned)(mo + (mo > 2 ? -3 : 9)) + 2) / 5 + (unsigned)d - 1;
        unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        int64_t days = (int64_t)era * 146097 + (int64_t)doe - 719468;      /* since 1970 */
        return days - 10957;                                                /* since 2000 */
    }
    return strtoll(s, NULL, 10);
}

static void load_preds(const char* path) {
    FILE* f = fopen(path, "r"); if (!f) { perror(path); exit(2); }
    char line[512];
    while (fgets(line, sizeof line, f) && g_npred < MAX_PRED) {
        if (line[0] == '#' || line[0] == '\n') continue;
        pred_t* p = &g_pred[g_npred];
        char vals[256];
        if (sscanf(line, "%31s %63s %3s %255s", p->group, p->col, p->op, vals) != 4) continue;
        p->nv = 0;
        for (char* t = strtok(vals, ","); t && p->nv < 8; t = strtok(NULL, ","))
            p->v[p->nv++] = parse_const(t, &p->sym_empty);
        printf("pred\t%s\t%s\t%s", p->group, p->col, p->op);
        for (int i = 0; i < p->nv; i++) printf("\t%lld", (long long)p->v[i]);
        printf("\n");
        g_npred++;
    }
    fclose(f);
}

/* Decision of one predicate over a range with logical min/max and nulls:
 * 0 = no row passes, 1 = every row passes, 2 = undecided.  Ranges with
 * bitmap nulls are never decided (conservative, as zone_cmp_decision);
 * SYM ranges decide only == / != / in (membership bounds). */
static int decide(const pred_t* p, int64_t mn, int64_t mx, bool has_nulls, bool is_sym) {
    if (has_nulls) return 2;
    bool eq_all = mn == mx;
    if (!strcmp(p->op, "==")) { int64_t c = p->v[0]; if (c < mn || c > mx) return 0; return eq_all && c == mn ? 1 : 2; }
    if (!strcmp(p->op, "!=")) { int64_t c = p->v[0]; if (c < mn || c > mx) return 1; return eq_all && c == mn ? 0 : 2; }
    if (!strcmp(p->op, "in")) {
        bool any = false;
        for (int i = 0; i < p->nv; i++) if (p->v[i] >= mn && p->v[i] <= mx) any = true;
        if (!any) return 0;
        if (eq_all) for (int i = 0; i < p->nv; i++) if (p->v[i] == mn) return 1;
        return 2;
    }
    if (is_sym) return 2;
    int64_t c = p->v[0];
    if (!strcmp(p->op, "<"))  return mx < c ? 1 : mn >= c ? 0 : 2;
    if (!strcmp(p->op, "<=")) return mx <= c ? 1 : mn > c ? 0 : 2;
    if (!strcmp(p->op, ">"))  return mn > c ? 1 : mx <= c ? 0 : 2;
    if (!strcmp(p->op, ">=")) return mn >= c ? 1 : mx < c ? 0 : 2;
    return 2;
}

/* Per predicate: per-vector decisions at chunk level and vector level. */
typedef struct { int8_t* chunk_dec; int8_t* vec_dec; int64_t nvec; int64_t* vec_rows; int64_t* vec_bytes; } pred_eval_t;
static pred_eval_t g_eval[MAX_PRED];

/* ---- totals ---- */
static int64_t g_raw_data, g_enc_data, g_index_bytes, g_index_kept, g_other, g_rows;
static int64_t g_sym_raw, g_sym_fsst;
static int64_t g_enc_count[10];

static void report_col(const char* name, int8_t type, int64_t raw, int64_t enc, int64_t idx) {
    printf("col\t%s\t%s\t%lld\t%lld\t%.2f\t%lld\n", name, ray_type_name(type),
           (long long)raw, (long long)enc, enc ? (double)raw / (double)enc : 0.0, (long long)idx);
}

/* Encode one column (one segment) and fill predicate evaluations for it. */
static bool g_skipped[MAX_PRED];
static int64_t estimate_column(ray_t* col, const char* name, int64_t vec_base, int64_t total_vecs) {
    ray_enc_kind_t k;
    if (!ray_enc_kind_of(col->type, col->attrs, &k)) {
        for (int p = 0; p < g_npred; p++)
            if (!strcmp(g_pred[p].col, name) && !g_skipped[p]) {
                g_skipped[p] = true;
                printf("pred_skipped\t%s\t%s\tcolumn type %s is not encodable\n", g_pred[p].group, name, ray_type_name(col->type));
            }
        return -1;
    }
    const void* data = ray_data(col);
    int64_t len = col->len;
    int64_t n_chunks = (len + RAY_ENC_CHUNK - 1) / RAY_ENC_CHUNK;
    int64_t bytes = 32 + 64 + 64 * n_chunks;                 /* header, enc header, directory */
    ray_enc_input_t* in = xmalloc(sizeof *in);
    int64_t dict_uses = 0;                                   /* vectors that chose DICT */

    /* VALUE_DICT from the first chunk (integer and temporal only). */
    ray_enc_dict_t dict = { NULL, 0 }; uint64_t* dv = NULL; uint32_t dn = 0;
    if (k.is_signed && len > 0) {
        int64_t m = len < RAY_ENC_CHUNK ? len : RAY_ENC_CHUNK;
        uint64_t* first = xmalloc(sizeof(uint64_t) * (size_t)m);
        for (int64_t s = 0; s < m; s += RAY_ENC_VEC) {
            int n = (int)(m - s < RAY_ENC_VEC ? m - s : RAY_ENC_VEC);
            ray_enc_load(data, col->type, col->attrs, s, n, in);
            memcpy(first + s, in->bits, sizeof(uint64_t) * (size_t)n);
        }
        int tr = ray_enc_train_dict(first, m, k, &dv, &dn);
        if (tr < 0) { fprintf(stderr, "out of memory training dictionary\n"); exit(1); }
        if (tr == 1) { dict.v = dv; dict.n = dn; }
        free(first);
    }

    for (int64_t ch = 0; ch < n_chunks; ch++) {
        int64_t c0 = ch * RAY_ENC_CHUNK, c1 = c0 + RAY_ENC_CHUNK < len ? c0 + RAY_ENC_CHUNK : len;
        int64_t cmin = INT64_MAX, cmax = INT64_MIN; bool cnull = false;
        bytes += 64 * 48;                                          /* 64 descriptors */
        for (int64_t s = c0; s < c1; s += RAY_ENC_VEC) {
            int n = (int)(c1 - s < RAY_ENC_VEC ? c1 - s : RAY_ENC_VEC);
            ray_enc_load(data, col->type, col->attrs, s, n, in);
            ray_enc_choice_t c = ray_enc_choose(in->bits, n, k, RAY_ENC_SLOT_TOP, 0, dict.n ? &dict : NULL);
            int64_t vb = c.bytes + (in->bitmap && in->n_nulls ? 128 : 0);
            bytes += vb;
            g_enc_count[c.enc]++;
            dict_uses += c.enc == RAY_ENC_DICT;
            ray_enc_stats_t st; ray_enc_stats(in->bits, n, k, &st);
            int64_t mn = ray_enc_logical(st.min, k), mx = ray_enc_logical(st.max, k);
            bool vnull = in->bitmap && in->n_nulls > 0;
            if (mn < cmin) cmin = mn;
            if (mx > cmax) cmax = mx;
            cnull |= vnull;
            int64_t vi = vec_base + s / RAY_ENC_VEC;
            for (int p = 0; p < g_npred; p++) {
                if (strcmp(g_pred[p].col, name)) continue;
                pred_eval_t* e = &g_eval[p];
                if (!e->vec_dec) {
                    e->nvec = total_vecs;
                    e->vec_dec = xcalloc((size_t)e->nvec, 1); e->chunk_dec = xcalloc((size_t)e->nvec, 1);
                    e->vec_rows = xcalloc((size_t)e->nvec, sizeof(int64_t)); e->vec_bytes = xcalloc((size_t)e->nvec, sizeof(int64_t));
                }
                e->vec_dec[vi] = (int8_t)decide(&g_pred[p], mn, mx, vnull, col->type == RAY_SYM);
                e->vec_rows[vi] = n; e->vec_bytes[vi] = vb;
            }
        }
        for (int p = 0; p < g_npred; p++) {
            if (strcmp(g_pred[p].col, name) || !g_eval[p].chunk_dec) continue;
            int d = decide(&g_pred[p], cmin, cmax, cnull, col->type == RAY_SYM);
            for (int64_t v = vec_base + c0 / RAY_ENC_VEC; v <= vec_base + (c1 - 1) / RAY_ENC_VEC; v++)
                g_eval[p].chunk_dec[v] = (int8_t)d;
        }
    }
    /* VALUE_DICT side data, charged only when a vector uses it: the side
     * table (u32 n_entries, u32 reserved, one 24-byte entry record = 32 B),
     * then the entry (u32 n, u32 reserved, n values, padded to 8) placed at
     * a 32-aligned offset. */
    if (dict_uses) {
        int64_t body = (16 + (int64_t)dn * k.T / 8 + 7) & ~(int64_t)7;
        bytes += 32 + ((body + 31) & ~(int64_t)31);
    }
    if (dv) ray_free_raw(dv);
    free(in);
    return bytes;
}

/* Index bytes of a column file: file size minus the payload (32-aligned). */
static int64_t index_bytes(const char* path, ray_t* col) {
    int64_t fs = fsize(path);
    if (col->type == RAY_STR || col->type == RAY_LIST) return 0;   /* not split in P0 */
    uint8_t esz = ray_sym_elem_size(col->type, col->attrs);
    int64_t payload = 32 + col->len * esz;
    int64_t region = (payload + 31) & ~(int64_t)31;
    return fs > region ? fs - region : 0;
}

static bool kept(const char* name, const char* keep) {
    if (!keep) return false;
    size_t n = strlen(name);
    for (const char* p = keep; (p = strstr(p, name)); p += n)
        if ((p == keep || p[-1] == ',') && (p[n] == ',' || p[n] == 0)) return true;
    return false;
}

/* Process one splayed directory (a table or one partition). */
/* Resolve a splayed directory to its published generation (or itself). */
static bool resolve_dir(const char* dir, char* out, size_t sz) {
    return ray_splay_resolve_dir(dir, out, sz) == RAY_OK;
}

static void process_splayed(const char* dir, const char* sym, const char* keep, int64_t vec_base, int64_t total_vecs) {
    char rdir[1024];
    if (!resolve_dir(dir, rdir, sizeof rdir)) { fprintf(stderr, "cannot resolve %s\n", dir); exit(2); }
    ray_t* t = ray_read_splayed(dir, sym);
    if (!t || RAY_IS_ERR(t)) { fprintf(stderr, "cannot read %s\n", dir); exit(2); }
    int64_t nc = ray_table_ncols(t);
    for (int64_t c = 0; c < nc; c++) {
        ray_t* col = ray_table_get_col_idx(t, c);
        ray_t* nm = ray_sym_str(ray_table_col_name(t, c));
        char name[128]; snprintf(name, sizeof name, "%.*s", (int)ray_str_len(nm), ray_str_ptr(nm));
        char path[1280]; snprintf(path, sizeof path, "%s/%s", rdir, name);
        int64_t fs = fsize(path), ib = index_bytes(path, col), raw = fs - ib;
        int64_t enc = estimate_column(col, name, vec_base, total_vecs);
        g_raw_data += raw; g_index_bytes += ib; if (kept(name, keep)) g_index_kept += ib;
        g_enc_data += enc >= 0 ? enc : raw;                 /* unencodable types stay raw */
        report_col(name, col->type, raw, enc >= 0 ? enc : raw, ib);
    }
    g_rows += ray_table_nrows(t);
    char p[1100]; snprintf(p, sizeof p, "%s/.d", rdir); g_other += fsize(p);
    ray_release(t);
}

/* ---- parallel decode of retained blocks ---- */
typedef struct {
    ray_fsst_table_t* t; uint8_t* arena; uint32_t* eoff; uint32_t* elen; uint32_t* l; int n;
} sym_blk_t;
typedef struct { sym_blk_t* blks; int nblk; int next; int bad; } sym_par_t;

static void* sym_par_worker(void* a) {
    sym_par_t* P = a;
    size_t dcap = 1 << 16; uint8_t* dbuf = xmalloc(dcap);
    for (;;) {
        int b = __atomic_fetch_add(&P->next, 1, __ATOMIC_RELAXED);
        if (b >= P->nblk) break;
        sym_blk_t* k = &P->blks[b];
        for (int i = 0; i < k->n; i++) {
            if ((size_t)k->l[i] > dcap) { while ((size_t)k->l[i] > dcap) dcap *= 2; free(dbuf); dbuf = xmalloc(dcap); }
            int64_t d = ray_fsst_decode(k->t, k->arena + k->eoff[i], k->elen[i], dbuf, dcap);
            if (d != (int64_t)k->l[i]) __atomic_store_n(&P->bad, 1, __ATOMIC_RELAXED);
        }
    }
    free(dbuf);
    return NULL;
}

/* ---- symfile estimate ---- */
/* The FSST `lengths` slot (U32) allows only RAW and FOR_BP: size one vector
 * of lengths as the smaller of the two plus its 8-byte child header. */
static int64_t fsst_lengths_bytes(const uint64_t* lv, int m) {
    ray_enc_kind_t ku = { 32, false };
    uint64_t base;
    int w = ray_enc_for_width(lv, m, ku, &base);
    int64_t raw = ray_enc_size_raw(m, 32), fo = ray_enc_size_for(w);
    return (raw < fo ? raw : fo) + 8;
}

static void estimate_sym(const char* sym) {
    ray_sym_domain_t* dom = ray_sym_domain_open(sym);
    if (!dom) { printf("sym\tnone\n"); return; }
    ray_sym_domain_raw_t r;
    if (!ray_sym_domain_raw_pin(dom, &r)) { printf("sym\tno-raw\n"); ray_sym_domain_release(dom); return; }
    int64_t raw_file = fsize(sym), snapshot = 8 * r.count, enc = 0;
    double dec_s = 0;
    const int64_t B = 65536;
    const uint8_t** s = xmalloc(sizeof(uint8_t*) * (size_t)B);
    uint32_t* l = xmalloc(sizeof(uint32_t) * (size_t)B);
    uint32_t* elen = xmalloc(sizeof(uint32_t) * (size_t)B);
    size_t* eoff = xmalloc(sizeof(size_t) * (size_t)B);
    size_t cap = 1 << 22, dcap = 1 << 16;
    uint8_t* arena = xmalloc(cap);
    uint8_t* dbuf = xmalloc(dcap);
    ray_fsst_table_t* t = xmalloc(sizeof *t);
    ray_fsst_index_t* ix = xmalloc(sizeof *ix);
    int64_t nblk_max = (r.count + B - 1) / B;
    sym_blk_t* keep = xmalloc(sizeof(sym_blk_t) * (size_t)(nblk_max ? nblk_max : 1));
    int nkeep = 0;
    for (int64_t b0 = 0; b0 < r.count; b0 += B) {
        int64_t n = r.count - b0 < B ? r.count - b0 : B;
        for (int64_t i = 0; i < n; i++) {
            size_t len; const char* p = ray_sym_domain_raw_str(&r, b0 + i, &len);
            s[i] = (const uint8_t*)p; l[i] = (uint32_t)len; snapshot += 4 + (int64_t)len;
        }
        ray_fsst_train(s, l, n, t);
        ray_fsst_index_build(t, ix);                    /* once per block */
        int64_t blk = ray_fsst_table_bytes(t);
        /* encode into a block arena; lengths packed per 1024 as U32 vectors */
        size_t used = 0;
        for (int64_t i = 0; i < n; i++) {
            if (used + 2 * (size_t)l[i] > cap) {
                while (used + 2 * (size_t)l[i] > cap) cap *= 2;
                arena = realloc(arena, cap); if (!arena) { fprintf(stderr, "out of memory\n"); exit(2); }
            }
            int64_t e = ray_fsst_encode_ix(t, ix, s[i], l[i], arena + used);
            eoff[i] = used; elen[i] = (uint32_t)e; used += (size_t)e;
        }
        blk += (int64_t)((used + 7) & ~(size_t)7) + 8;
        for (int64_t v = 0; v < n; v += RAY_ENC_VEC) {
            int m = (int)(n - v < RAY_ENC_VEC ? n - v : RAY_ENC_VEC);
            uint64_t lv[RAY_ENC_VEC]; for (int i = 0; i < m; i++) lv[i] = elen[v + i];
            blk += fsst_lengths_bytes(lv, m);
        }
        enc += blk;
        double t0 = now_s();
        for (int64_t i = 0; i < n; i++) {
            if ((size_t)l[i] > dcap) { while ((size_t)l[i] > dcap) dcap *= 2; free(dbuf); dbuf = xmalloc(dcap); }
            int64_t d = ray_fsst_decode(t, arena + eoff[i], elen[i], dbuf, dcap);
            if (d != (int64_t)l[i] || memcmp(dbuf, s[i], l[i]) != 0) { fprintf(stderr, "fsst mismatch at %lld\n", (long long)(b0 + i)); exit(3); }
        }
        dec_s += now_s() - t0;
        sym_blk_t* k = &keep[nkeep++];
        k->n = (int)n;
        k->t = xmalloc(sizeof *t); memcpy(k->t, t, sizeof *t);
        k->arena = xmalloc(used ? used : 1); memcpy(k->arena, arena, used);
        k->eoff = xmalloc(sizeof(uint32_t) * (size_t)n); k->elen = xmalloc(sizeof(uint32_t) * (size_t)n);
        k->l = xmalloc(sizeof(uint32_t) * (size_t)n);
        for (int64_t i = 0; i < n; i++) { k->eoff[i] = (uint32_t)eoff[i]; k->elen[i] = elen[i]; k->l[i] = l[i]; }
    }
    /* parallel decode of every retained block on all online CPUs */
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN); if (ncpu < 1) ncpu = 1;
    sym_par_t P = { keep, nkeep, 0, 0 };
    pthread_t* th = xmalloc(sizeof(pthread_t) * (size_t)ncpu);
    double tp = now_s();
    long started = 0;
    for (long i = 0; i < ncpu; i++) {
        if (pthread_create(&th[i], NULL, sym_par_worker, &P) != 0) break;
        started++;
    }
    if (started < ncpu) {
        fprintf(stderr, "warning: started %ld of %ld decode threads\n", started, ncpu);
        sym_par_worker(&P);          /* the caller drains whatever blocks remain */
    }
    for (long i = 0; i < started; i++) pthread_join(th[i], NULL);
    ncpu = started < ncpu ? started + 1 : ncpu;
    double par_s = now_s() - tp;
    if (P.bad) { fprintf(stderr, "parallel fsst decode length mismatch\n"); exit(3); }
    for (int i = 0; i < nkeep; i++) { free(keep[i].t); free(keep[i].arena); free(keep[i].eoff); free(keep[i].elen); free(keep[i].l); }
    free(keep); free(th);
    printf("sym\tfile=%lld\tfsst=%lld\tratio=%.2f\tsnapshot=%lld\tdecode_s=%.3f\tsym_parallel_decode_s=%.3f\tthreads=%ld\tcount=%lld\n",
           (long long)raw_file, (long long)enc, enc ? (double)raw_file / (double)enc : 0,
           (long long)snapshot, dec_s, par_s, ncpu, (long long)r.count);
    free(s); free(l); free(elen); free(eoff); free(arena); free(dbuf); free(t); free(ix);
    ray_sym_domain_raw_unpin(dom);
    ray_sym_domain_release(dom);
    g_sym_raw = raw_file; g_sym_fsst = enc;
}

static void report_groups(void) {
    char seen[64][32]; int ns = 0;
    for (int p = 0; p < g_npred; p++) {
        bool dup = false; for (int i = 0; i < ns; i++) if (!strcmp(seen[i], g_pred[p].group)) dup = true;
        if (dup || ns >= 64) continue;
        strcpy(seen[ns++], g_pred[p].group);
        int64_t nvec = 0; for (int q = 0; q < g_npred; q++) if (!strcmp(g_pred[q].group, g_pred[p].group) && g_eval[q].nvec) nvec = g_eval[q].nvec;
        if (!nvec) { printf("group\t%s\tno-columns\n", g_pred[p].group); continue; }
        int64_t rows = 0, chunk_dec = 0, vec_dec = 0, und = 0, bytes_all = 0, bytes_und = 0;
        for (int64_t v = 0; v < nvec; v++) {
            int cd = 1, vd = 1; int64_t vr = 0, vb = 0;    /* AND of {0 none, 1 all, 2 undecided} */
            for (int q = 0; q < g_npred; q++) {
                if (strcmp(g_pred[q].group, g_pred[p].group) || !g_eval[q].nvec) continue;
                int c = g_eval[q].chunk_dec[v], d = g_eval[q].vec_dec[v];
                cd = (cd == 0 || c == 0) ? 0 : (cd == 2 || c == 2) ? 2 : 1;
                vd = (vd == 0 || d == 0) ? 0 : (vd == 2 || d == 2) ? 2 : 1;
                vr = g_eval[q].vec_rows[v];
                vb += g_eval[q].vec_bytes[v];
            }
            bytes_all += vb;
            if (vd == 2 && cd == 2) bytes_und += vb;       /* final combined decisions */
            rows += vr;
            if (cd != 2) chunk_dec += vr; else if (vd != 2) vec_dec += vr; else und += vr;
        }
        printf("group\t%s\trows=%lld\tchunk_decided=%.4f\tvector_decided=%.4f\tundecided=%.4f\tpred_bytes=%lld\tpred_bytes_undecided=%lld\n",
               g_pred[p].group, (long long)rows, rows ? (double)chunk_dec / (double)rows : 0,
               rows ? (double)vec_dec / (double)rows : 0, rows ? (double)und / (double)rows : 0,
               (long long)bytes_all, (long long)bytes_und);
    }
}

int main(int argc, char** argv) {
    const char *db = NULL, *table = NULL, *sym = NULL, *preds = NULL, *keep = NULL;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--db")) db = argv[i + 1];
        else if (!strcmp(argv[i], "--table")) table = argv[i + 1];
        else if (!strcmp(argv[i], "--sym")) sym = argv[i + 1];
        else if (!strcmp(argv[i], "--preds")) preds = argv[i + 1];
        else if (!strcmp(argv[i], "--keep-index")) keep = argv[i + 1];
    }
    if (!db) { fprintf(stderr, "usage: compress_estimate --db DIR [--table NAME] [--sym PATH] [--preds FILE] [--keep-index COL,...]\n"); return 2; }
    ray_heap_init(); (void)ray_sym_init();
    char symbuf[1024];
    if (!sym) { snprintf(symbuf, sizeof symbuf, "%s/.sym", db); sym = symbuf; }
    if (preds) load_preds(preds);
    printf("col\tname\ttype\traw\tencoded\tratio\tindex\n");

    char rdb[1024], d0[1100];
    if (!resolve_dir(db, rdb, sizeof rdb)) snprintf(rdb, sizeof rdb, "%s", db);
    snprintf(d0, sizeof d0, "%s/.d", rdb);
    if (fsize(d0) > 0) {
        ray_t* t = ray_read_splayed(db, sym);
        if (!t || RAY_IS_ERR(t)) { fprintf(stderr, "cannot read %s\n", db); return 2; }
        int64_t rows = ray_table_nrows(t); ray_release(t);
        process_splayed(db, sym, keep, 0, (rows + RAY_ENC_VEC - 1) / RAY_ENC_VEC);
    } else {
        if (!table) { fprintf(stderr, "parted root needs --table\n"); return 2; }
        printf("note\tparted root: the engine's index sites never consult parted segment indexes; "
               "skip rates and index sizes show what the files allow, not what the engine uses today\n");
        /* partitions: subdirectories in name order, each <part>/<table>/ */
        struct dirent** ents; int ne = scandir(db, &ents, NULL, alphasort);
        if (ne < 0) { perror(db); return 2; }
        int64_t total = 0, base = 0;
        for (int pass = 0; pass < 2; pass++) {
            for (int i = 0; i < ne; i++) {
                if (ents[i]->d_name[0] == '.') continue;
                char pd[1024]; snprintf(pd, sizeof pd, "%s/%s/%s", db, ents[i]->d_name, table);
                char rpd[1024], pdd[1100];
                if (!resolve_dir(pd, rpd, sizeof rpd)) continue;
                snprintf(pdd, sizeof pdd, "%s/.d", rpd);
                if (fsize(pdd) <= 0) continue;
                ray_t* t = ray_read_splayed(pd, sym);
                if (!t || RAY_IS_ERR(t)) { fprintf(stderr, "cannot read %s\n", pd); return 2; }
                int64_t r = ray_table_nrows(t); ray_release(t);
                int64_t nv = (r + RAY_ENC_VEC - 1) / RAY_ENC_VEC;   /* each partition owns its vector slots */
                if (pass == 0) total += nv;
                else { process_splayed(pd, sym, keep, base, total); base += nv; }
            }
        }
        for (int i = 0; i < ne; i++) free(ents[i]);
        free(ents);
    }
    estimate_sym(sym);
    report_groups();

    static const char* encname[10] = { "RAW","CONST","FOR_BP","DELTA_BP","RLE","DICT","ALP","ALP_RD","STR_DICT","FSST" };
    int64_t nv = 0; for (int e = 0; e < 10; e++) nv += g_enc_count[e];
    for (int e = 0; e < 10; e++) if (g_enc_count[e]) printf("mix_fraction_of_vectors\t%s\t%.4f\n", encname[e], (double)g_enc_count[e] / (double)nv);

    int64_t before = g_raw_data + g_index_bytes + g_sym_raw + g_other;
    struct { const char* name; int64_t size; } sc[4] = {
        { "today",                       before },
        { "columns encoded",             g_enc_data + g_index_bytes + g_sym_raw + g_other },
        { "columns + sym fsst",          g_enc_data + g_index_bytes + g_sym_fsst + g_other },
        { "columns + sym fsst + kept ix",g_enc_data + g_index_kept + g_sym_fsst + g_other },
    };
    printf("breakdown\trows=%lld\tdata=%lld\tindex=%lld\tsym=%lld\tother=%lld\ttotal=%lld\n",
           (long long)g_rows, (long long)g_raw_data, (long long)g_index_bytes, (long long)g_sym_raw,
           (long long)g_other, (long long)before);
    /* Combined-score factor from size alone: the size term of the site's
     * combined score has weight 0.1, so factor = (after / before)^0.1. */
    for (int i = 0; i < 4; i++)
        printf("scenario\t%s\tbytes=%lld\tsize_ratio=%.3f\tscore_factor=%.4f\n", sc[i].name,
               (long long)sc[i].size, (double)before / (double)sc[i].size, pow((double)sc[i].size / (double)before, 0.1));
    ray_sym_destroy(); ray_heap_destroy();
    return 0;
}
