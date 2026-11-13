/*
 * readval.c -- data-only reader (see readval.h).
 *
 * One recursive-descent pass over a private, NUL-terminated copy of the text.
 * No AST and no evaluation: each production builds its value directly, through
 * the reader-local allocation ledger (read_budget.h), so every block is
 * admitted against the allocation budget before it exists and a failed read
 * releases everything by closing the ledger.
 *
 * Symbols are staged, not interned.  A symbol occurrence becomes a fixup (the
 * address of the id cell, the staged text); when the whole document has been
 * read and validated, ray_sym_publish_values commits every new name at once
 * and the fixups are filled without allocating.  A rejected read therefore
 * leaves the intern table as it found it.
 */
#include "lang/readval.h"
#include "lang/read_budget.h"
#include "lang/parse.h"
#include "lang/eval.h"          /* ATTR_QUOTED */
#include "table/sym.h"
#include "vec/str.h"
#include "mem/heap.h"   /* RAY_ATTR_HAS_NULLS */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#define EMPTY_SYM ((size_t)-1)

ray_read_limits_t ray_read_limits_default(void) {
    return (ray_read_limits_t){
        .max_input        = (size_t)8 << 20,
        .max_depth        = 128,
        .max_nodes        = 1000000,
        .max_grammar      = 1000000,
        .max_symbol_bytes = (size_t)1 << 20,
        .max_alloc        = (size_t)64 << 20,
        .max_symbols      = UINT32_MAX,
        .inject_fail      = 0,
    };
}

typedef struct { void* p; size_t len, cap; } rd_buf_t;
typedef struct { size_t off, len; } sref_t;
typedef struct { int64_t* slot; size_t sidx; } fix_t;

typedef struct {
    const char *src, *p, *end;
    ray_read_limits_t lim;
    ray_read_budget_t bud;
    size_t gnodes, dnodes, symbytes, depth;
    rd_buf_t sbuf;      /* decoded symbol bytes                       */
    rd_buf_t srefs;     /* sref_t per staged symbol                   */
    rd_buf_t fix;       /* fix_t per symbol id cell                   */
    rd_buf_t scratch;   /* one decoded string                         */
    rd_buf_t cells;     /* staged elements of one vector literal      */
    rd_buf_t pool;      /* pooled string bytes of one STR vector      */
    rd_buf_t kids;      /* stack of children of the open lists        */
    rd_buf_t ks;        /* stack of staged key names of open dict literals */
    rd_buf_t kk;        /* stack of string keys of open dict literals */
    const char* err_code;
    char err_msg[192];
    ray_t* last_symvec;     /* the SYM vector most recently built...  */
    size_t last_symvec_fix; /* ...and the index of its first fixup    */
} rd_t;

/* ---- errors ------------------------------------------------------------ */

static void* fail(rd_t* rd, const char* code, const char* fmt, ...) {
    if (rd->err_code) return NULL;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(rd->err_msg, sizeof rd->err_msg, fmt, ap);
    va_end(ap);
    rd->err_code = code;
    return NULL;
}

static void* fail_at(rd_t* rd, const char* fmt, ...) {
    if (rd->err_code) return NULL;
    char what[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(what, sizeof what, fmt, ap);
    va_end(ap);
    return fail(rd, "parse", "read-value: %s at byte %zu", what, (size_t)(rd->p - rd->src));
}

static void* fail_budget(rd_t* rd) {
    switch (rd->bud.status) {
    case RAY_READ_LIMIT:
        return fail(rd, "limit", "read-value: construction allocation limit of %zu bytes exceeded", rd->lim.max_alloc);
    case RAY_READ_OOM:     return fail(rd, "oom", "read-value: out of memory");
    default:               return fail(rd, "domain", "read-value: input cannot be represented");
    }
}

static bool over(rd_t* rd, size_t* counter, size_t n, size_t cap, const char* name) {
    if (n > cap - *counter) {
        fail(rd, "limit", "read-value: %s limit of %zu exceeded", name, cap);
        return false;
    }
    *counter += n;
    return true;
}
#define GRAM(rd, n) over(rd, &(rd)->gnodes, (n), (rd)->lim.max_grammar, "grammar-node")
#define DATA(rd, n) over(rd, &(rd)->dnodes, (n), (rd)->lim.max_nodes, "data-node")

/* ---- charged growable buffers ----------------------------------------- */

static bool buf_need(rd_t* rd, rd_buf_t* b, size_t extra) {
    if (extra > SIZE_MAX / 2 - b->len) { fail(rd, "limit", "read-value: size overflow"); return false; }
    size_t need = b->len + extra;
    if (need <= b->cap) return true;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < need) cap *= 2;
    void* np = b->p ? ray_read_sys_resize(&rd->bud, b->p, b->len, cap)
                    : ray_read_sys_alloc(&rd->bud, cap);
    if (!np) { fail_budget(rd); return false; }
    b->p = np;
    b->cap = cap;
    return true;
}

static bool buf_push(rd_t* rd, rd_buf_t* b, const void* data, size_t n) {
    if (!buf_need(rd, b, n)) return false;
    if (n) memcpy((char*)b->p + b->len, data, n);
    b->len += n;
    return true;
}

/* ---- blocks ------------------------------------------------------------ */

static ray_t* blk(rd_t* rd, size_t data_bytes, int8_t type) {
    ray_t* v = ray_read_alloc(&rd->bud, data_bytes);
    if (!v) return fail_budget(rd);
    v->type = type;
    v->attrs = 0;
    memset(v->aux, 0, 16);
    v->i64 = 0;
    return v;
}

static bool add_fix(rd_t* rd, int64_t* slot, size_t sidx) {
    fix_t f = { slot, sidx };
    return buf_push(rd, &rd->fix, &f, sizeof f);
}

/* A symbol occurrence: its decoded bytes are charged now and staged. */
static bool stage_sym(rd_t* rd, const char* s, size_t n, size_t* sidx) {
    if (n == 0) { *sidx = EMPTY_SYM; return true; }
    if (n > rd->lim.max_symbol_bytes - rd->symbytes) {
        fail(rd, "limit", "read-value: symbol-byte limit of %zu exceeded", rd->lim.max_symbol_bytes);
        return false;
    }
    rd->symbytes += n;
    sref_t r = { rd->sbuf.len, n };
    if (!buf_push(rd, &rd->sbuf, s, n)) return false;
    *sidx = rd->srefs.len / sizeof(sref_t);
    return buf_push(rd, &rd->srefs, &r, sizeof r);
}

/* ---- scanning ---------------------------------------------------------- */

static void skip_ws(rd_t* rd) {
    for (;;) {
        char c = *rd->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') rd->p++;
        else if (c == ';') { while (rd->p < rd->end && *rd->p != '\n') rd->p++; }
        else return;
    }
}

static bool at_end(rd_t* rd) { return rd->p >= rd->end; }

static bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static bool is_symc(char c)  { return is_alpha(c) || is_digit(c) || c == '_' || c == '.' || c == '-'; }

/* A token ends at whitespace, a closer, a comment or the end. */
static bool token_end(rd_t* rd) {
    char c = *rd->p;
    return rd->p >= rd->end || c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == ')' || c == ']' || c == '}' || c == ';';
}

/* Decodes the string literal at rd->p into rd->scratch (not NUL-terminated). */
static bool read_string(rd_t* rd) {
    rd->p++;  /* opening quote */
    rd->scratch.len = 0;
    for (;;) {
        if (at_end(rd)) { fail_at(rd, "unterminated string"); return false; }
        char c = *rd->p++;
        if (c == '"') return true;
        if (c == '\\') {
            if (at_end(rd)) { fail_at(rd, "unterminated string"); return false; }
            char e = *rd->p++;
            switch (e) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case '\\': c = '\\'; break;
            case '"': c = '"'; break;
            case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': {
                unsigned v = (unsigned)(e - '0');
                for (int k = 0; k < 2 && !at_end(rd) && *rd->p >= '0' && *rd->p <= '7'; k++)
                    v = (v << 3) | (unsigned)(*rd->p++ - '0');
                if (v > 255) { fail_at(rd, "octal escape out of range"); return false; }
                c = (char)v;
                break;
            }
            default:
                rd->p--;
                fail_at(rd, "unknown string escape \\%c", e);
                return false;
            }
        }
        if (!buf_push(rd, &rd->scratch, &c, 1)) return false;
    }
}

/* '[A-Za-z0-9_.-]*  at rd->p == '\''.  Points into the source: no copy. */
static bool read_quoted_sym(rd_t* rd, const char** s, size_t* n) {
    rd->p++;
    const char* start = rd->p;
    while (!at_end(rd) && is_symc(*rd->p)) rd->p++;
    if (!at_end(rd) && (*rd->p == '\'' || *rd->p == '\\')) {
        fail_at(rd, "character literals are not data");
        return false;
    }
    *s = start;
    *n = (size_t)(rd->p - start);
    return true;
}

/* ---- atoms ------------------------------------------------------------- */

static ray_t* new_atom(rd_t* rd, int8_t type) {
    if (!DATA(rd, 1)) return NULL;
    return blk(rd, 0, type);
}

static ray_t* guid_atom(rd_t* rd, const uint8_t g[16], bool null) {
    ray_t* bytes = ray_read_alloc(&rd->bud, 16);
    if (!bytes) return fail_budget(rd);
    bytes->type = RAY_U8;
    bytes->len = 16;
    bytes->attrs = 0;
    memset(bytes->aux, 0, 16);
    memcpy(ray_data(bytes), g, 16);
    ray_t* v = new_atom(rd, -RAY_GUID);
    if (!v) return NULL;
    ray_retain(bytes);
    v->obj = bytes;
    if (null) v->aux[0] |= 1;
    return v;
}

static bool parse_guid(const char* s, size_t n, uint8_t out[16]) {
    if (n != 36) return false;
    int nib = 0;
    for (size_t j = 0; j < n; j++) {
        char c = s[j];
        if (j == 8 || j == 13 || j == 18 || j == 23) { if (c != '-') return false; continue; }
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else return false;
        if ((nib & 1) == 0) out[nib >> 1] = (uint8_t)(v << 4);
        else out[nib >> 1] |= (uint8_t)v;
        nib++;
    }
    return true;
}

/* Fills the value of a scanned literal into the payload union of `v`. */
static void set_lit(ray_t* v, const ray_scalar_lit_t* l) {
    if (l->null) {
        switch (l->type) {
        case -RAY_F64: v->f64 = NULL_F64; break;
        case -RAY_F32: v->f64 = (double)NULL_F32; break;
        case -RAY_I16: v->i16 = NULL_I16; break;
        case -RAY_I32: case -RAY_DATE: case -RAY_TIME: v->i32 = NULL_I32; break;
        default: v->i64 = NULL_I64; break;
        }
        v->aux[0] |= 1;
        return;
    }
    switch (l->type) {
    case -RAY_U8:  v->u8 = (uint8_t)l->i; break;
    case -RAY_I16: v->i16 = (int16_t)l->i; break;
    case -RAY_I32: case -RAY_DATE: case -RAY_TIME: v->i32 = (int32_t)l->i; break;
    case -RAY_F32: case -RAY_F64: v->f64 = l->f; break;
    default: v->i64 = l->i; break;
    }
}

/* Scans a number-like literal at rd->p and refuses what the data model has no
 * value for: an integer too large for I64, a float too large for its width,
 * and a non-null value that would read as the type's null. */
static bool scan_lit(rd_t* rd, ray_scalar_lit_t* l) {
    const char* start = rd->p;
    const char* pos = rd->p;
    ray_t* err = ray_parse_scalar_literal(&pos, l);
    if (err) {
        const char* m = ray_error_msg();
        const char* c = ray_err_code(err);
        char msg[128];
        snprintf(msg, sizeof msg, "%s", m && *m ? m : c);
        bool domain = c && strcmp(c, "domain") == 0;
        ray_error_free(err);
        ray_error_clear();
        if (domain) fail(rd, "domain", "read-value: %s at byte %zu", msg, (size_t)(rd->p - rd->src));
        else fail_at(rd, "%s", msg);
        return false;
    }
    rd->p = pos;
    if (!token_end(rd)) { fail_at(rd, "unexpected character directly after a literal"); return false; }
    if (l->widened) { rd->p = start; fail_at(rd, "integer literal out of the I64 range"); return false; }
    if (l->nonfinite) { rd->p = start; fail_at(rd, "float literal out of range"); return false; }
    if (!l->null) {
        bool bad = false;
        switch (l->type) {
        case -RAY_I64: case -RAY_TIMESTAMP: case -RAY_TIMESPAN: bad = l->i == INT64_MIN; break;
        case -RAY_I32: case -RAY_DATE: case -RAY_TIME: bad = l->i == INT32_MIN; break;
        case -RAY_F32: bad = isnan(l->f); break;
        case -RAY_F64: bad = isnan(l->f); break;
        default: break;
        }
        if (bad) { rd->p = start; fail_at(rd, "literal out of range for its type"); return false; }
    }
    return true;
}

static int8_t tag_type(const char* s, size_t n) {
    static const struct { const char* name; int8_t type; } tags[] = {
        {"B8", RAY_BOOL}, {"U8", RAY_U8}, {"I16", RAY_I16}, {"I32", RAY_I32}, {"I64", RAY_I64},
        {"F32", RAY_F32}, {"F64", RAY_F64}, {"DATE", RAY_DATE}, {"TIME", RAY_TIME},
        {"TIMESTAMP", RAY_TIMESTAMP}, {"TIMESPAN", RAY_TIMESPAN}, {"GUID", RAY_GUID},
        {"SYM", RAY_SYM}, {"STR", RAY_STR},
    };
    for (size_t i = 0; i < sizeof tags / sizeof *tags; i++)
        if (strlen(tags[i].name) == n && memcmp(tags[i].name, s, n) == 0) return tags[i].type;
    return 0;
}

/* ---- vectors ----------------------------------------------------------- */

static uint8_t cell_size(int8_t vt) {
    switch (vt) {
    case RAY_BOOL: case RAY_U8: return 1;
    case RAY_I16: return 2;
    case RAY_I32: case RAY_DATE: case RAY_TIME: case RAY_F32: return 4;
    default: return 8;   /* I64 family, F64, and staged SYM / STR cells */
    }
}

static ray_t* vec_blk(rd_t* rd, int8_t vt, size_t n, size_t es) {
    if (n > SIZE_MAX / es) return fail(rd, "limit", "read-value: vector too long");
    ray_t* v = blk(rd, n * es, vt);
    if (!v) return NULL;
    v->len = (int64_t)n;
    return v;
}

static ray_t* build_sym_vec(rd_t* rd, size_t n, const size_t* sidx) {
    ray_t* v = vec_blk(rd, RAY_SYM, n, sizeof(int64_t));
    if (!v) return NULL;
    v->attrs = RAY_SYM_W64;
    v->sym_domain = ray_sym_runtime_domain();
    int64_t* d = (int64_t*)ray_data(v);
    rd->last_symvec = v;
    rd->last_symvec_fix = rd->fix.len / sizeof(fix_t);
    for (size_t i = 0; i < n; i++) {
        d[i] = 0;
        if (sidx[i] == EMPTY_SYM) v->attrs |= RAY_ATTR_HAS_NULLS;
        if (!add_fix(rd, &d[i], sidx[i])) return NULL;
    }
    return v;
}

static ray_t* build_str_vec(rd_t* rd, size_t n, const uint32_t* cells, const char* pool) {
    size_t pool_bytes = 0;
    for (size_t i = 0; i < n; i++)
        if (cells[2 * i + 1] > RAY_STR_INLINE_MAX) pool_bytes += cells[2 * i + 1];
    ray_t* v = vec_blk(rd, RAY_STR, n, sizeof(ray_str_t));
    if (!v) return NULL;
    ray_t* pb = NULL;
    if (pool_bytes) {
        pb = blk(rd, pool_bytes, RAY_U8);
        if (!pb) return NULL;
        pb->len = (int64_t)pool_bytes;
        ray_retain(pb);
        v->str_pool = pb;
    }
    ray_str_t* el = (ray_str_t*)ray_data(v);
    size_t poff = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t len = cells[2 * i + 1];
        const char* s = pool + cells[2 * i];
        memset(&el[i], 0, sizeof el[i]);
        el[i].len = len;
        if (len == 0) v->attrs |= RAY_ATTR_HAS_NULLS;
        else if (len <= RAY_STR_INLINE_MAX) memcpy(el[i].data, s, len);
        else {
            char* base = (char*)ray_data(pb);
            memcpy(base + poff, s, len);
            memcpy(el[i].prefix, s, 4);
            el[i].pool_off = (uint32_t)poff;
            ray_str_t_cache_hash(&el[i], base);
            poff += len;
        }
    }
    return v;
}

/* `[` elem* `]`: a typed vector from literal elements of one type. */
static ray_t* read_vec_literal(rd_t* rd) {
    rd->p++;
    int8_t vt = 0;
    size_t n = 0, es = 0;
    bool nulls = false;
    rd->cells.len = 0;
    rd->pool.len = 0;
    for (;;) {
        skip_ws(rd);
        if (at_end(rd)) return fail_at(rd, "unterminated vector literal");
        if (*rd->p == ']') { rd->p++; break; }
        if (!GRAM(rd, 1)) return NULL;
        char c = *rd->p;
        int8_t et;
        ray_scalar_lit_t lit = {0};
        const char* sym = NULL;
        size_t symn = 0;
        if (c == '"') {
            if (!read_string(rd)) return NULL;
            et = RAY_STR;
        } else if (c == '\'') {
            if (!read_quoted_sym(rd, &sym, &symn)) return NULL;
            et = RAY_SYM;
        } else if (is_digit(c) || (c == '-' && is_digit(rd->p[1]))) {
            if (!scan_lit(rd, &lit)) return NULL;
            et = (int8_t)-lit.type;
        } else if (is_alpha(c)) {
            const char* s = rd->p;
            while (!at_end(rd) && is_symc(*rd->p)) rd->p++;
            size_t k = (size_t)(rd->p - s);
            if ((k == 4 && !memcmp(s, "true", 4)) || (k == 5 && !memcmp(s, "false", 5))) {
                et = RAY_BOOL;
                bool b = k == 4;
                if (vt && vt != et) return fail_at(rd, "vector elements must all have one type");
                if (!vt) { vt = et; es = cell_size(vt); }
                if (!token_end(rd)) return fail_at(rd, "unexpected character after a name");
                if (!buf_push(rd, &rd->cells, &b, 1)) return NULL;
                n++;
                continue;
            }
            if (k == 4 && !memcmp(s, "null", 4)) return fail_at(rd, "null is not a vector element");
            sym = s; symn = k;   /* a bare name inside [...] is a symbol literal */
            et = RAY_SYM;
            if (!token_end(rd)) return fail_at(rd, "unexpected character after a name");
        } else {
            return fail_at(rd, "unexpected character '%c' in a vector literal", c);
        }
        if (et == RAY_GUID) return fail_at(rd, "GUID elements need (as 'GUID (list ...))");
        if (vt && vt != et) return fail_at(rd, "vector elements must all have one type");
        if (!vt) { vt = et; es = cell_size(vt); }
        if (et == RAY_STR) {
            if (rd->pool.len > UINT32_MAX - rd->scratch.len) return fail(rd, "limit", "read-value: string vector pool too large");
            uint32_t cell[2] = { (uint32_t)rd->pool.len, (uint32_t)rd->scratch.len };
            if (!buf_push(rd, &rd->pool, rd->scratch.p, rd->scratch.len)) return NULL;
            if (!buf_push(rd, &rd->cells, cell, sizeof cell)) return NULL;
        } else if (et == RAY_SYM) {
            size_t sidx;
            if (!stage_sym(rd, sym, symn, &sidx)) return NULL;
            if (!buf_push(rd, &rd->cells, &sidx, sizeof sidx)) return NULL;
        } else {
            union { uint8_t u8; int16_t i16; int32_t i32; float f32; int64_t i64; double f64; } cell = { .i64 = 0 };
            if (lit.null) {
                nulls = true;
                switch (et) {
                case RAY_F64: cell.f64 = NULL_F64; break;
                case RAY_F32: cell.f32 = NULL_F32; break;
                case RAY_I16: cell.i16 = NULL_I16; break;
                case RAY_I32: case RAY_DATE: case RAY_TIME: cell.i32 = NULL_I32; break;
                default: cell.i64 = NULL_I64; break;
                }
            } else {
                switch (et) {
                case RAY_U8: cell.u8 = (uint8_t)lit.i; break;
                case RAY_I16: cell.i16 = (int16_t)lit.i; break;
                case RAY_I32: case RAY_DATE: case RAY_TIME: cell.i32 = (int32_t)lit.i; break;
                case RAY_F32: cell.f32 = (float)lit.f; break;
                case RAY_F64: cell.f64 = lit.f; break;
                default: cell.i64 = lit.i; break;
                }
            }
            if (!buf_push(rd, &rd->cells, &cell, es)) return NULL;
        }
        n++;
    }
    if (n == 0) return fail_at(rd, "an empty vector needs its type: write (as 'I64 [])");
    if (!DATA(rd, n + 1)) return NULL;
    if (vt == RAY_SYM) return build_sym_vec(rd, n, (const size_t*)rd->cells.p);
    if (vt == RAY_STR) return build_str_vec(rd, n, (const uint32_t*)rd->cells.p, (const char*)rd->pool.p);
    ray_t* v = vec_blk(rd, vt, n, es);
    if (!v) return NULL;
    memcpy(ray_data(v), rd->cells.p, n * es);
    if (nulls) v->attrs |= RAY_ATTR_HAS_NULLS;
    return v;
}

static ray_t* empty_vec(rd_t* rd, int8_t vt) {
    if (!DATA(rd, 1)) return NULL;
    if (vt == RAY_SYM) return build_sym_vec(rd, 0, NULL);
    ray_t* v = vec_blk(rd, vt, 0, vt == RAY_STR ? sizeof(ray_str_t) : cell_size(vt));
    return v;
}

/* ---- containers -------------------------------------------------------- */

static bool is_collection(ray_t* v) { return ray_is_vec(v) || v->type == RAY_LIST; }

static ray_t* read_value(rd_t* rd);

static bool expect_close(rd_t* rd) {
    skip_ws(rd);
    if (at_end(rd) || *rd->p != ')') { fail_at(rd, "expected ')'"); return false; }
    rd->p++;
    return true;
}

/* A list of already-read children kids[base..], as one LIST block. */
static ray_t* finish_list(rd_t* rd, size_t base) {
    ray_t** k = (ray_t**)rd->kids.p + base;
    size_t n = rd->kids.len / sizeof(ray_t*) - base;
    ray_t* v = ray_read_list(&rd->bud, k, n);
    rd->kids.len = base * sizeof(ray_t*);
    return v ? v : fail_budget(rd);
}

static ray_t* read_list_body(rd_t* rd) {
    size_t base = rd->kids.len / sizeof(ray_t*);
    for (;;) {
        skip_ws(rd);
        if (at_end(rd)) return fail_at(rd, "unterminated list");
        if (*rd->p == ')') { rd->p++; break; }
        ray_t* c = read_value(rd);
        if (!c) return NULL;
        if (!buf_push(rd, &rd->kids, &c, sizeof c)) return NULL;
    }
    if (!DATA(rd, 1)) return NULL;
    return finish_list(rd, base);
}

/* `(list "a" "b")` or `["a" "b"]` of string literals, as staged symbols. */
static ray_t* read_sym_strings(rd_t* rd) {
    bool paren = *rd->p == '(';
    if (paren) {
        rd->p++;
        skip_ws(rd);
        if (strncmp(rd->p, "list", 4) != 0 || !(rd->p[4] == ' ' || rd->p[4] == ')' || rd->p[4] == '\n' || rd->p[4] == '\t'))
            return fail_at(rd, "expected (list ...) of strings");
        rd->p += 4;
        if (!GRAM(rd, 2)) return NULL;
    } else if (*rd->p == '[') {
        rd->p++;
    } else return fail_at(rd, "expected a list of strings");
    size_t n = 0;
    rd->cells.len = 0;
    for (;;) {
        skip_ws(rd);
        if (at_end(rd)) return fail_at(rd, "unterminated list");
        if (*rd->p == (paren ? ')' : ']')) { rd->p++; break; }
        if (*rd->p != '"') return fail_at(rd, "expected a string");
        if (!GRAM(rd, 1) || !read_string(rd)) return NULL;
        size_t sidx;
        if (!stage_sym(rd, (const char*)rd->scratch.p, rd->scratch.len, &sidx)) return NULL;
        if (!buf_push(rd, &rd->cells, &sidx, sizeof sidx)) return NULL;
        n++;
    }
    if (!DATA(rd, n + 1)) return NULL;
    return build_sym_vec(rd, n, (const size_t*)rd->cells.p);
}

/* `(list (as 'GUID "...") 0Ng ...)`: a GUID vector. */
static ray_t* read_guid_list(rd_t* rd) {
    if (*rd->p != '(') return fail_at(rd, "expected (list ...) of GUIDs");
    rd->p++;
    skip_ws(rd);
    if (strncmp(rd->p, "list", 4) != 0) return fail_at(rd, "expected (list ...) of GUIDs");
    rd->p += 4;
    if (!GRAM(rd, 2)) return NULL;
    size_t n = 0;
    rd->cells.len = 0;
    bool nulls = false;
    for (;;) {
        skip_ws(rd);
        if (at_end(rd)) return fail_at(rd, "unterminated list");
        if (*rd->p == ')') { rd->p++; break; }
        uint8_t g[16] = {0};
        if (!GRAM(rd, 1)) return NULL;
        if (strncmp(rd->p, "0Ng", 3) == 0) {
            rd->p += 3;
            nulls = true;
        } else {
            if (*rd->p != '(') return fail_at(rd, "expected (as 'GUID \"...\") or 0Ng");
            rd->p++;
            skip_ws(rd);
            if (strncmp(rd->p, "as", 2) != 0) return fail_at(rd, "expected (as 'GUID \"...\")");
            rd->p += 2;
            skip_ws(rd);
            if (strncmp(rd->p, "'GUID", 5) != 0) return fail_at(rd, "expected 'GUID");
            rd->p += 5;
            skip_ws(rd);
            if (*rd->p != '"') return fail_at(rd, "expected a GUID string");
            if (!GRAM(rd, 3) || !read_string(rd)) return NULL;
            if (!parse_guid((const char*)rd->scratch.p, rd->scratch.len, g)) return fail_at(rd, "invalid GUID text");
            if (!expect_close(rd)) return NULL;
        }
        if (!buf_push(rd, &rd->cells, g, 16)) return NULL;
        n++;
    }
    if (!DATA(rd, n + 1)) return NULL;
    ray_t* v = vec_blk(rd, RAY_GUID, n, 16);
    if (!v) return NULL;
    if (n) memcpy(ray_data(v), rd->cells.p, n * 16);
    for (size_t i = 0; i < n && !nulls; i++) {
        const uint8_t* g = (const uint8_t*)ray_data(v) + i * 16;
        bool zero = true;
        for (int k = 0; k < 16; k++) if (g[k]) { zero = false; break; }
        nulls = zero;
    }
    if (nulls) v->attrs |= RAY_ATTR_HAS_NULLS;   /* an all-zero GUID is the null */
    return v;
}

static ray_t* read_as(rd_t* rd) {
    skip_ws(rd);
    if (*rd->p != '\'') return fail_at(rd, "expected a quoted type tag, as in (as 'I64 [])");
    const char* t;
    size_t tn;
    const char* save = rd->p;
    rd->p++;
    t = rd->p;
    while (!at_end(rd) && (is_alpha(*rd->p) || is_digit(*rd->p))) rd->p++;
    tn = (size_t)(rd->p - t);
    int8_t vt = tag_type(t, tn);
    if (!vt) { rd->p = save; return fail_at(rd, "unknown type tag"); }
    if (!GRAM(rd, 2)) return NULL;
    skip_ws(rd);
    ray_t* out = NULL;
    if (*rd->p == '[') {
        const char* open = rd->p;
        rd->p++;
        skip_ws(rd);
        if (*rd->p == ']') { rd->p++; out = empty_vec(rd, vt); }
        else if (vt == RAY_SYM) { rd->p = open; out = read_sym_strings(rd); }
        else return fail_at(rd, "as takes an empty vector [] here");
    } else if (vt == RAY_SYM && *rd->p == '"') {
        if (!GRAM(rd, 1) || !read_string(rd)) return NULL;
        size_t sidx;
        if (!stage_sym(rd, (const char*)rd->scratch.p, rd->scratch.len, &sidx)) return NULL;
        ray_t* a = new_atom(rd, -RAY_SYM);
        if (!a) return NULL;
        a->attrs = ATTR_QUOTED;
        if (!add_fix(rd, &a->i64, sidx)) return NULL;
        out = a;
    } else if (vt == RAY_SYM && *rd->p == '(') {
        out = read_sym_strings(rd);
    } else if (vt == RAY_GUID && *rd->p == '"') {
        uint8_t g[16];
        if (!GRAM(rd, 1) || !read_string(rd)) return NULL;
        if (!parse_guid((const char*)rd->scratch.p, rd->scratch.len, g)) return fail_at(rd, "invalid GUID text");
        out = guid_atom(rd, g, false);
    } else if (vt == RAY_GUID && *rd->p == '(') {
        out = read_guid_list(rd);
    } else {
        return fail_at(rd, "unsupported argument for as");
    }
    if (!out) return NULL;
    return expect_close(rd) ? out : NULL;
}

static ray_t* make_dict(rd_t* rd, ray_t* k, ray_t* v) {
    if (!is_collection(k) || !is_collection(v)) return fail(rd, "domain", "read-value: dict needs two vectors or lists");
    if (k->len != v->len) return fail(rd, "domain", "read-value: dict key and value counts differ (%lld, %lld)", (long long)k->len, (long long)v->len);
    ray_t* d = blk(rd, 2 * sizeof(ray_t*), RAY_DICT);
    if (!d) return NULL;
    d->len = 2;
    ray_retain(k); ray_retain(v);
    ((ray_t**)ray_data(d))[0] = k;
    ((ray_t**)ray_data(d))[1] = v;   /* not-a-table */
    return d;
}

static ray_t* read_dict_call(rd_t* rd) {
    ray_t* k = read_value(rd);
    if (!k) return NULL;
    ray_t* v = read_value(rd);
    if (!v) return NULL;
    if (!expect_close(rd)) return NULL;
    if (!DATA(rd, 1)) return NULL;
    return make_dict(rd, k, v);
}

static ray_t* read_table_call(rd_t* rd) {
    ray_t* names = read_value(rd);
    if (!names) return NULL;
    bool names_ok = names->type == RAY_SYM && rd->last_symvec == names;
    size_t fix_start = rd->last_symvec_fix;
    ray_t* cols = read_value(rd);
    if (!cols) return NULL;
    if (!expect_close(rd)) return NULL;
    if (!names_ok)
        return fail(rd, "domain", "read-value: table column names must be a symbol vector");
    if (cols->type != RAY_LIST)
        return fail(rd, "domain", "read-value: table columns must be a list");
    if (names->len != cols->len)
        return fail(rd, "domain", "read-value: table has %lld names and %lld columns", (long long)names->len, (long long)cols->len);
    int64_t rows = -1;
    for (int64_t i = 0; i < cols->len; i++) {
        ray_t* c = ((ray_t**)ray_data(cols))[i];
        if (!is_collection(c)) return fail(rd, "domain", "read-value: table column %lld is not a vector or list", (long long)i);
        if (rows >= 0 && c->len != rows)
            return fail(rd, "domain", "read-value: table columns have different lengths (%lld, %lld)", (long long)rows, (long long)c->len);
        rows = c->len;
    }
    if (!DATA(rd, 1)) return NULL;
    ray_t* schema = vec_blk(rd, RAY_I64, (size_t)names->len, sizeof(int64_t));
    if (!schema) return NULL;
    int64_t* sd = (int64_t*)ray_data(schema);
    for (int64_t i = 0; i < names->len; i++) {
        sd[i] = 0;
        const fix_t* f = (const fix_t*)rd->fix.p + fix_start + (size_t)i;
        if (!add_fix(rd, &sd[i], f->sidx)) return NULL;
    }
    ray_t* t = blk(rd, 2 * sizeof(ray_t*), RAY_TABLE);
    if (!t) return NULL;
    t->len = 2;
    ray_retain(schema); ray_retain(cols);
    ((ray_t**)ray_data(t))[0] = schema;
    ((ray_t**)ray_data(t))[1] = cols;   /* plain-table */
    return t;
}

/* `{k: v ...}`: keys are bare names or strings, values are data.  Values are
 * kept as a LIST, as the language's own dict literal does. */
static ray_t* read_dict_literal(rd_t* rd) {
    rd->p++;
    size_t vbase = rd->kids.len / sizeof(ray_t*);
    size_t sbase = rd->ks.len / sizeof(size_t);
    size_t kbase = rd->kk.len / sizeof(ray_t*);
    size_t n = 0;
    int kind = 0;   /* 1 name keys, 2 string keys */
    for (;;) {
        skip_ws(rd);
        if (at_end(rd)) return fail_at(rd, "unterminated dict literal");
        if (*rd->p == '}') { rd->p++; break; }
        if (!GRAM(rd, 1)) return NULL;
        if (*rd->p == '"') {
            if (kind == 1) return fail_at(rd, "dict keys must be all names or all strings");
            kind = 2;
            if (!read_string(rd) || !DATA(rd, 1)) return NULL;
            ray_t* key = ray_read_string(&rd->bud, (const char*)rd->scratch.p, rd->scratch.len);
            if (!key) return fail_budget(rd);
            if (!buf_push(rd, &rd->kk, &key, sizeof key)) return NULL;
        } else if (is_alpha(*rd->p) || *rd->p == '_') {
            if (kind == 2) return fail_at(rd, "dict keys must be all names or all strings");
            kind = 1;
            const char* s = rd->p;
            while (!at_end(rd) && (is_alpha(*rd->p) || is_digit(*rd->p) || *rd->p == '_' || *rd->p == '-')) rd->p++;
            size_t sidx;
            if (!stage_sym(rd, s, (size_t)(rd->p - s), &sidx)) return NULL;
            if (!buf_push(rd, &rd->ks, &sidx, sizeof sidx)) return NULL;
        } else {
            return fail_at(rd, "expected a dict key");
        }
        skip_ws(rd);
        if (*rd->p != ':') return fail_at(rd, "expected ':' after a dict key");
        rd->p++;
        ray_t* val = read_value(rd);
        if (!val) return NULL;
        if (!buf_push(rd, &rd->kids, &val, sizeof val)) return NULL;
        n++;
    }
    /* The same occurrences as (dict KEYS (list VALS)): the dict, the key
     * vector (a container and its n elements; string keys were counted one by
     * one as they were read) and the value list.  The values counted as they
     * were read. */
    if (!DATA(rd, kind == 2 ? 3 : n + 3)) return NULL;
    ray_t* keys;
    if (kind == 1) {
        keys = build_sym_vec(rd, n, (const size_t*)rd->ks.p + sbase);
    } else if (kind == 2) {
        ray_t** ka = (ray_t**)rd->kk.p + kbase;
        rd->cells.len = 0;
        rd->pool.len = 0;
        for (size_t i = 0; i < n; i++) {
            size_t sl = ray_str_len(ka[i]);
            if (rd->pool.len > UINT32_MAX - sl) return fail(rd, "limit", "read-value: string vector pool too large");
            uint32_t cell[2] = { (uint32_t)rd->pool.len, (uint32_t)sl };
            if (!buf_push(rd, &rd->pool, ray_str_ptr(ka[i]), sl) || !buf_push(rd, &rd->cells, cell, sizeof cell)) return NULL;
        }
        keys = build_str_vec(rd, n, (const uint32_t*)rd->cells.p, (const char*)rd->pool.p);
    } else {
        keys = ray_read_list(&rd->bud, NULL, 0);
        if (!keys) fail_budget(rd);
    }
    rd->ks.len = sbase * sizeof(size_t);
    rd->kk.len = kbase * sizeof(ray_t*);
    if (!keys) return NULL;
    ray_t* vals = finish_list(rd, vbase);
    if (!vals) return NULL;
    return make_dict(rd, keys, vals);
}

/* ---- values ------------------------------------------------------------ */

static ray_t* read_value(rd_t* rd) {
    skip_ws(rd);
    if (at_end(rd)) return fail_at(rd, "expected a value");
    if (!GRAM(rd, 1)) return NULL;
    if (++rd->depth > rd->lim.max_depth) return fail(rd, "limit", "read-value: nesting depth limit of %zu exceeded", rd->lim.max_depth);
    ray_t* out = NULL;
    char c = *rd->p;
    if (c == '(') {
        rd->p++;
        skip_ws(rd);
        if (*rd->p == ')') { rd->p++; out = (DATA(rd, 1)) ? finish_list(rd, rd->kids.len / sizeof(ray_t*)) : NULL; }
        else {
            const char* h = rd->p;
            while (!at_end(rd) && (is_alpha(*rd->p) || is_digit(*rd->p) || *rd->p == '_' || *rd->p == '-')) rd->p++;
            size_t hn = (size_t)(rd->p - h);
            if (hn == 4 && !memcmp(h, "list", 4)) out = read_list_body(rd);
            else if (hn == 4 && !memcmp(h, "dict", 4)) out = read_dict_call(rd);
            else if (hn == 5 && !memcmp(h, "table", 5)) out = read_table_call(rd);
            else if (hn == 2 && !memcmp(h, "as", 2)) out = read_as(rd);
            else { rd->p = h; out = fail_at(rd, "only list, dict, table and as can be called here"); }
        }
    } else if (c == '[') {
        out = read_vec_literal(rd);
    } else if (c == '{') {
        out = read_dict_literal(rd);
    } else if (c == '"') {
        if (read_string(rd) && DATA(rd, 1)) {
            out = ray_read_string(&rd->bud, (const char*)rd->scratch.p, rd->scratch.len);
            if (!out) fail_budget(rd);
        }
    } else if (c == '\'') {
        const char* s; size_t n; size_t sidx;
        if (read_quoted_sym(rd, &s, &n) && stage_sym(rd, s, n, &sidx)) {
            ray_t* a = new_atom(rd, -RAY_SYM);
            if (a) {
                a->attrs = ATTR_QUOTED;
                if (add_fix(rd, &a->i64, sidx)) out = a;
            }
        }
    } else if (is_digit(c) || (c == '-' && is_digit(rd->p[1]))) {
        ray_scalar_lit_t lit;
        if (scan_lit(rd, &lit)) {
            if (lit.type == -RAY_GUID) {   /* 0Ng: the null GUID atom */
                static const uint8_t zero[16] = {0};
                out = guid_atom(rd, zero, true);
            } else {
                ray_t* a = new_atom(rd, (int8_t)lit.type);
                if (a) { set_lit(a, &lit); out = a; }
            }
        }
    } else if (is_alpha(c)) {
        const char* s = rd->p;
        while (!at_end(rd) && is_symc(*rd->p)) rd->p++;
        size_t k = (size_t)(rd->p - s);
        if (k == 4 && !memcmp(s, "null", 4) && token_end(rd)) { if (DATA(rd, 1)) out = RAY_NULL_OBJ; }
        else if (((k == 4 && !memcmp(s, "true", 4)) || (k == 5 && !memcmp(s, "false", 5))) && token_end(rd)) {
            ray_t* a = new_atom(rd, -RAY_BOOL);
            if (a) { a->b8 = k == 4; out = a; }
        } else { rd->p = s; out = fail_at(rd, "names are not data; quote a symbol as 'name"); }
    } else {
        out = fail_at(rd, "unexpected character '%c'", c);
    }
    rd->depth--;
    return out;
}

/* ---- entry ------------------------------------------------------------- */

static void rd_close(rd_t* rd) { ray_read_budget_close(&rd->bud); }

static ray_t* rd_error(rd_t* rd) {
    const char* code = rd->err_code ? rd->err_code : "domain";
    char msg[sizeof rd->err_msg];
    snprintf(msg, sizeof msg, "%s", rd->err_code ? rd->err_msg : "read-value: failed");
    rd_close(rd);
    return ray_error(code, "%s", msg);
}

ray_t* ray_read_value(const char* text, size_t len, const ray_read_limits_t* limits) {
    rd_t rd;
    memset(&rd, 0, sizeof rd);
    rd.lim = limits ? *limits : ray_read_limits_default();
    if (rd.lim.max_depth > RAY_READ_DEPTH_CAP) rd.lim.max_depth = RAY_READ_DEPTH_CAP;
    if (!text && len) return ray_error("domain", "read-value: no input");
    if (len > rd.lim.max_input)
        return ray_error("limit", "read-value: input of %zu bytes exceeds the limit of %zu", len, rd.lim.max_input);
    if (!ray_read_budget_init(&rd.bud, rd.lim.max_alloc))
        return ray_error("limit", "read-value: allocation limit of %zu bytes is too small", rd.lim.max_alloc);
    if (rd.lim.inject_fail) rd.bud.fail_after = rd.lim.inject_fail - 1;
    char* copy = ray_read_sys_alloc(&rd.bud, len + 1);
    if (!copy) { fail_budget(&rd); return rd_error(&rd); }
    if (len) memcpy(copy, text, len);
    copy[len] = 0;
    for (size_t i = 0; i < len; i++)
        if (!copy[i]) { rd.src = rd.p = copy; rd.end = copy + len; rd.p += i; fail_at(&rd, "NUL byte in input"); return rd_error(&rd); }
    rd.src = rd.p = copy;
    rd.end = copy + len;

    ray_t* root = read_value(&rd);
    if (root) {
        skip_ws(&rd);
        if (!at_end(&rd)) { fail_at(&rd, "trailing input after the value"); root = NULL; }
    }
    if (!root) return rd_error(&rd);

    /* Commit the staged symbols as one transaction, then fill the id cells. */
    size_t nsym = rd.srefs.len / sizeof(sref_t);
    int64_t* ids = NULL;
    if (nsym) {
        if (!ray_sym_materialize_lazy()) { fail(&rd, "oom", "read-value: the persisted symbol table could not be loaded"); return rd_error(&rd); }
        const char** strs = ray_read_sys_alloc(&rd.bud, nsym * sizeof *strs);
        size_t* lens = ray_read_sys_alloc(&rd.bud, nsym * sizeof *lens);
        ids = ray_read_sys_alloc(&rd.bud, nsym * sizeof *ids);
        if (!strs || !lens || !ids) { fail_budget(&rd); return rd_error(&rd); }
        const sref_t* refs = (const sref_t*)rd.srefs.p;
        for (size_t i = 0; i < nsym; i++) { strs[i] = (const char*)rd.sbuf.p + refs[i].off; lens[i] = refs[i].len; }
        if (!ray_sym_publish_values(&rd.bud, strs, lens, nsym, rd.lim.max_symbols, ids)) {
            if (rd.bud.status == RAY_READ_LIMIT) fail(&rd, "limit", "read-value: symbol table limit exceeded");
            else fail_budget(&rd);
            return rd_error(&rd);
        }
    }
    const fix_t* fx = (const fix_t*)rd.fix.p;
    for (size_t i = 0, nf = rd.fix.len / sizeof(fix_t); i < nf; i++)
        *fx[i].slot = fx[i].sidx == EMPTY_SYM ? 0 : ids[fx[i].sidx];

    ray_retain(root);
    rd_close(&rd);
    return root;
}
