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

/*
 * json.c -- JSON text in and out (see json.h, docs/docs/namespaces/json.md).
 *
 * The reader is one recursive-descent pass over the bytes of one document,
 * strict RFC 8259: one value, whitespace around it, no comments, no trailing
 * commas, no leading zeros, no raw control characters or invalid UTF-8 in a
 * string, no lone surrogate.  It builds values as it goes and, on the first
 * error, releases what it built: a malformed document is an error naming its
 * byte, never a partial value.  The depth, node and allocation counters are
 * charged before the value they count is built, so a hostile document is
 * stopped by a cap, never by the stack or the heap.  Nothing is evaluated
 * and nothing is interned: object keys are strings.
 *
 * Values: object -> dict (STR keys, LIST values; a repeated key keeps its
 * first position and its last value), array -> a typed vector when every
 * element is one kind of scalar (json_collapse), else a LIST; an integer
 * (no fraction, no exponent) in I64 range -> I64, any other number -> F64;
 * true/false -> bool; null -> null; string -> STR.
 *
 * (.json.get s path) walks the same grammar, building only the value at the
 * path and validating the rest of the document without building it.
 */
#include "io/json.h"
#include "lang/internal.h"
#include "ops/walkmemo.h"       /* ray_dup_t */
#include "lang/readval.h"      /* ray_read_limits_t, ray_read_limits_apply */
#include "lang/format.h"       /* ray_type_name, ray_fmt */
#include "ops/internal.h"      /* pool_cancelled, RAY_IS_PARTED */
#include "ops/hash.h"          /* ray_hash_bytes */
#include "core/numparse.h"     /* ray_parse_f64, ray_strtod_c, ray_snprintf_c */
#include "table/sym.h"
#include "core/platform.h"   /* ray_stack_short: the depth guard */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ==========================================================================
 * Reader
 * ========================================================================== */

typedef struct {
    const uint8_t *s, *p, *end;   /* the document, the cursor                 */
    const char* who;              /* ".json.parse" / ".json.get"              */
    int64_t row;                  /* element of a vector input, -1 for an atom */
    const char* unit;             /* what `row` counts: "element", or "path"
                                   * for one document read by a vector of paths */
    size_t depth, max_depth;
    size_t nodes, max_nodes;
    size_t bytes, max_alloc;      /* bytes of the values built so far        */
    ray_t* err;                   /* the first error (owned)                 */
    char* buf;                    /* one decoded string with escapes          */
    size_t buf_cap;
} jp_t;

static void* jp_fail(jp_t* jp, const char* code, const char* fmt, ...) {
    if (jp->err) return NULL;
    char what[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(what, sizeof what, fmt, ap);
    va_end(ap);
    size_t at = (size_t)(jp->p - jp->s);
    if (jp->row >= 0)
        jp->err = ray_error(code, "%s: %s at byte %zu of %s %lld", jp->who, what, at,
                            jp->unit ? jp->unit : "element", (long long)jp->row);
    else
        jp->err = ray_error(code, "%s: %s at byte %zu", jp->who, what, at);
    return NULL;
}

/* Charge `nbytes` against the allocation cap without counting a node. */
static bool jp_bytes(jp_t* jp, size_t nbytes) {
    if (nbytes > jp->max_alloc - jp->bytes || jp->bytes > jp->max_alloc) {
        jp_fail(jp, "limit", "allocation limit of %zu bytes exceeded", jp->max_alloc);
        return false;
    }
    jp->bytes += nbytes;
    return true;
}

static bool jp_charge(jp_t* jp, size_t nbytes) {
    if (++jp->nodes > jp->max_nodes) {
        jp_fail(jp, "limit", "node limit of %zu exceeded", jp->max_nodes);
        return false;
    }
    return jp_bytes(jp, nbytes);
}

#define JP_ATOM_BYTES 32   /* one atom block, as estimated before it is built */

static ray_t* jp_scalar(jp_t* jp, ray_t* v, size_t est);

static inline void jp_ws(jp_t* jp) {
    while (jp->p < jp->end && (*jp->p == ' ' || *jp->p == '\t' || *jp->p == '\n' || *jp->p == '\r'))
        jp->p++;
}

static void* jp_unexpected(jp_t* jp, const char* expected) {
    if (jp->p >= jp->end) return jp_fail(jp, "parse", "unexpected end of input, expected %s", expected);
    uint8_t c = *jp->p;
    if (c >= 0x20 && c < 0x7f)
        return jp_fail(jp, "parse", "unexpected character '%c', expected %s", c, expected);
    return jp_fail(jp, "parse", "unexpected byte 0x%02x, expected %s", c, expected);
}

/* Length of the UTF-8 sequence at p (< end) if it is well formed (shortest
 * form, no surrogate, at most U+10FFFF), else 0. */
static size_t utf8_seq(const uint8_t* p, const uint8_t* end) {
    uint8_t c = p[0];
    size_t n;
    uint32_t cp;
    if (c < 0x80) return 1;
    if (c >= 0xc2 && c <= 0xdf) { n = 2; cp = c & 0x1f; }
    else if (c >= 0xe0 && c <= 0xef) { n = 3; cp = c & 0x0f; }
    else if (c >= 0xf0 && c <= 0xf4) { n = 4; cp = c & 0x07; }
    else return 0;
    if ((size_t)(end - p) < n) return 0;
    for (size_t i = 1; i < n; i++) {
        if ((p[i] & 0xc0) != 0x80) return 0;
        cp = (cp << 6) | (p[i] & 0x3f);
    }
    if ((n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) || cp > 0x10ffff ||
        (cp >= 0xd800 && cp <= 0xdfff)) return 0;
    return n;
}

static int hexval(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* The four hex digits after "\u" at jp->p (jp->p on the 'u'). */
static int64_t jp_hex4(jp_t* jp) {
    uint32_t v = 0;
    for (int i = 1; i <= 4; i++) {
        if (jp->p + i >= jp->end) { jp->p = jp->end; jp_fail(jp, "parse", "unexpected end of input in a \\u escape"); return -1; }
        int h = hexval(jp->p[i]);
        if (h < 0) { jp->p += i; jp_fail(jp, "parse", "invalid hex digit in a \\u escape"); return -1; }
        v = (v << 4) | (uint32_t)h;
    }
    jp->p += 5;
    return v;
}

static bool jp_bytes(jp_t* jp, size_t nbytes);

/* The decoded-string buffer grown to n bytes, its growth charged. */
static bool jp_buf_need(jp_t* jp, size_t n) {
    if (n <= jp->buf_cap) return true;
    size_t cap = jp->buf_cap ? jp->buf_cap : 64;
    while (cap < n) cap *= 2;
    if (!jp_bytes(jp, cap - jp->buf_cap)) return false;
    char* nb = (char*)ray_realloc_raw(jp->buf, cap);
    if (!nb) { jp_fail(jp, "oom", "out of memory"); return false; }
    jp->buf = nb;
    jp->buf_cap = cap;
    return true;
}

/* A string at jp->p (on the opening quote).  *out points at its decoded
 * bytes: into the document when it has no escape, else into jp->buf, valid
 * until the next string is read. */
static bool jp_string(jp_t* jp, const char** out, size_t* out_len) {
    jp->p++;                                   /* opening quote */
    const uint8_t* start = jp->p;
    const uint8_t* q = jp->p;
    bool esc = false;
    while (q < jp->end && *q != '"') {
        if (*q == '\\') { esc = true; q += 2; continue; }
        q++;
    }
    if (!esc) {
        /* validate in place */
        while (jp->p < jp->end && *jp->p != '"') {
            uint8_t c = *jp->p;
            if (c < 0x20) return jp_fail(jp, "parse", "control character 0x%02x in a string", c), false;
            size_t k = utf8_seq(jp->p, jp->end);
            if (!k) return jp_fail(jp, "parse", "invalid UTF-8 in a string"), false;
            jp->p += k;
        }
        if (jp->p >= jp->end) return jp_fail(jp, "parse", "unterminated string"), false;
        *out = (const char*)start;
        *out_len = (size_t)(jp->p - start);
        jp->p++;                               /* closing quote */
        return true;
    }
    /* decoded bytes are never longer than the raw ones */
    size_t raw = (size_t)((q < jp->end ? q : jp->end) - start);
    if (!jp_buf_need(jp, raw + 4)) return false;
    size_t len = 0;
    while (jp->p < jp->end && *jp->p != '"') {
        uint8_t c = *jp->p;
        if (c == '\\') {
            if (jp->p + 1 >= jp->end) { jp->p = jp->end; return jp_fail(jp, "parse", "unterminated string"), false; }
            uint8_t e = jp->p[1];
            char o;
            switch (e) {
            case '"':  o = '"';  break;
            case '\\': o = '\\'; break;
            case '/':  o = '/';  break;
            case 'b':  o = '\b'; break;
            case 'f':  o = '\f'; break;
            case 'n':  o = '\n'; break;
            case 'r':  o = '\r'; break;
            case 't':  o = '\t'; break;
            case 'u': {
                const uint8_t* at = jp->p;
                jp->p++;                       /* on the 'u' */
                int64_t cp = jp_hex4(jp);
                if (cp < 0) return false;
                if (cp >= 0xdc00 && cp <= 0xdfff) { jp->p = at; return jp_fail(jp, "parse", "lone low surrogate \\u%04llx", (long long)cp), false; }
                if (cp >= 0xd800 && cp <= 0xdbff) {
                    if (jp->end - jp->p < 2 || jp->p[0] != '\\' || jp->p[1] != 'u') {
                        jp->p = at;
                        return jp_fail(jp, "parse", "lone high surrogate \\u%04llx", (long long)cp), false;
                    }
                    jp->p++;                   /* on the second 'u' */
                    int64_t lo = jp_hex4(jp);
                    if (lo < 0) return false;
                    if (lo < 0xdc00 || lo > 0xdfff) {
                        jp->p = at;
                        return jp_fail(jp, "parse", "lone high surrogate \\u%04llx", (long long)cp), false;
                    }
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                }
                char* d = jp->buf + len;
                if (cp < 0x80) { d[0] = (char)cp; len += 1; }
                else if (cp < 0x800) { d[0] = (char)(0xc0 | (cp >> 6)); d[1] = (char)(0x80 | (cp & 0x3f)); len += 2; }
                else if (cp < 0x10000) {
                    d[0] = (char)(0xe0 | (cp >> 12)); d[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
                    d[2] = (char)(0x80 | (cp & 0x3f)); len += 3;
                } else {
                    d[0] = (char)(0xf0 | (cp >> 18)); d[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
                    d[2] = (char)(0x80 | ((cp >> 6) & 0x3f)); d[3] = (char)(0x80 | (cp & 0x3f)); len += 4;
                }
                continue;
            }
            default:
                jp->p++;
                return jp_fail(jp, "parse", "invalid escape in a string"), false;
            }
            jp->buf[len++] = o;
            jp->p += 2;
            continue;
        }
        if (c < 0x20) return jp_fail(jp, "parse", "control character 0x%02x in a string", c), false;
        size_t k = utf8_seq(jp->p, jp->end);
        if (!k) return jp_fail(jp, "parse", "invalid UTF-8 in a string"), false;
        memcpy(jp->buf + len, jp->p, k);
        len += k;
        jp->p += k;
    }
    if (jp->p >= jp->end) return jp_fail(jp, "parse", "unterminated string"), false;
    jp->p++;                                   /* closing quote */
    *out = jp->buf;
    *out_len = len;
    return true;
}

/* A number at jp->p.  An integer (no fraction, no exponent) in I64 range is
 * an I64, except -9223372036854775808, which is the I64 null: that one and
 * every other number is an F64.  A number past the F64 range is a `domain`
 * error (the F64 domain holds no infinity).  `build` false only validates. */
static ray_t* jp_number(jp_t* jp, bool build) {
    const uint8_t* st = jp->p;
    const uint8_t* q = jp->p;
    bool neg = false, frac = false;
    if (q < jp->end && *q == '-') { neg = true; q++; }
    if (q >= jp->end || *q < '0' || *q > '9') { jp->p = q; return jp_unexpected(jp, "a digit"); }
    if (*q == '0') {
        q++;
        if (q < jp->end && *q >= '0' && *q <= '9') { jp->p = q; return jp_fail(jp, "parse", "leading zero in a number"); }
    } else {
        while (q < jp->end && *q >= '0' && *q <= '9') q++;
    }
    if (q < jp->end && *q == '.') {
        frac = true;
        q++;
        if (q >= jp->end || *q < '0' || *q > '9') { jp->p = q; return jp_unexpected(jp, "a digit after the decimal point"); }
        while (q < jp->end && *q >= '0' && *q <= '9') q++;
    }
    if (q < jp->end && (*q == 'e' || *q == 'E')) {
        frac = true;
        q++;
        if (q < jp->end && (*q == '+' || *q == '-')) q++;
        if (q >= jp->end || *q < '0' || *q > '9') { jp->p = q; return jp_unexpected(jp, "a digit in the exponent"); }
        while (q < jp->end && *q >= '0' && *q <= '9') q++;
    }
    size_t len = (size_t)(q - st);
    if (!frac) {
        const uint8_t* d = st + (neg ? 1 : 0);
        uint64_t v = 0;
        bool fits = true;
        for (; d < q; d++) {
            uint64_t dig = (uint64_t)(*d - '0');
            if (v > (UINT64_MAX - dig) / 10) { fits = false; break; }
            v = v * 10 + dig;
        }
        /* -2^63 is the I64 null: an F64 like every number past the range */
        if (fits && (neg ? v < ((uint64_t)1 << 63) : v <= (uint64_t)INT64_MAX)) {
            jp->p = q;
            if (!build) return RAY_NULL_OBJ;
            if (!jp_charge(jp, JP_ATOM_BYTES)) return NULL;
            return jp_scalar(jp, ray_i64(neg ? -(int64_t)v : (int64_t)v), JP_ATOM_BYTES);
        }
    }
    double f = 0.0;
    if (ray_parse_f64((const char*)st, len, &f) != len || !isfinite(f))
        return jp_fail(jp, "domain", "number out of the F64 range");
    jp->p = q;
    if (!build) return RAY_NULL_OBJ;
    if (!jp_charge(jp, JP_ATOM_BYTES)) return NULL;
    return jp_scalar(jp, ray_f64(f), JP_ATOM_BYTES);
}

static bool jp_literal(jp_t* jp, const char* word, size_t n) {
    if ((size_t)(jp->end - jp->p) < n || memcmp(jp->p, word, n) != 0) {
        /* point at the first byte that differs */
        size_t i = 0;
        while (i < n && jp->p + i < jp->end && jp->p[i] == (uint8_t)word[i]) i++;
        jp->p += i;
        jp_unexpected(jp, "a value");
        return false;
    }
    jp->p += n;
    return true;
}

/* The kind of scalar a built element is, for json_collapse. */
enum { JK_OTHER, JK_NULL, JK_INT, JK_FLOAT, JK_BOOL, JK_STR };

static int json_kind(ray_t* v) {
    if (!v || RAY_IS_NULL(v)) return JK_NULL;
    switch (v->type) {
    case -RAY_I64:  return JK_INT;
    case -RAY_F64:  return JK_FLOAT;
    case -RAY_BOOL: return JK_BOOL;
    case -RAY_STR:  return JK_STR;
    default:        return JK_OTHER;
    }
}

/* A LIST of JSON values as the vector it collapses to, or the LIST itself
 * (consumed either way):
 *   every element a string or null        -> STR, a null as "" (the STR null)
 *   every element an integer or null      -> I64, a null as 0Nl
 *   every element a number or null, some
 *     of them not integers                -> F64, a null as 0Nf
 *   every element a bool                  -> BOOL (it has no null)
 *   anything else, and only nulls         -> the LIST
 * `all_null_str` gives a STR of nulls for a LIST of nulls only (.json.get:
 * a field absent from every row). */
static ray_t* json_collapse(ray_t* list, bool all_null_str) {
    int64_t n = list->len;
    if (n == 0 && all_null_str) {
        ray_release(list);
        ray_t* e = ray_vec_new(RAY_STR, 0);
        return e ? e : ray_error("oom", NULL);
    }
    if (n == 0) return list;
    ray_t** it = (ray_t**)ray_data(list);
    bool any_int = false, any_float = false, any_bool = false, any_str = false, any_null = false;
    for (int64_t i = 0; i < n; i++) {
        switch (json_kind(it[i])) {
        case JK_NULL:  any_null = true;  break;
        case JK_INT:   any_int = true;   break;
        case JK_FLOAT: any_float = true; break;
        case JK_BOOL:  any_bool = true;  break;
        case JK_STR:   any_str = true;   break;
        default:       return list;
        }
    }
    int kinds = (any_int || any_float) + any_bool + any_str;
    if (kinds > 1) return list;
    if (kinds == 0 && !all_null_str) return list;
    if (any_bool && any_null) return list;
    ray_t* out;
    if (any_str || kinds == 0) {
        out = ray_vec_new(RAY_STR, n);
        for (int64_t i = 0; out && !RAY_IS_ERR(out) && i < n; i++) {
            bool str = json_kind(it[i]) == JK_STR;
            out = ray_str_vec_append(out, str ? ray_str_ptr(it[i]) : "", str ? ray_str_len(it[i]) : 0);
        }
    } else if (any_bool) {
        out = ray_vec_new(RAY_BOOL, n);
        if (out && !RAY_IS_ERR(out)) {
            uint8_t* d = (uint8_t*)ray_data(out);
            for (int64_t i = 0; i < n; i++) d[i] = it[i]->b8 ? 1 : 0;
            out->len = n;
        }
    } else if (any_float) {
        out = ray_vec_new(RAY_F64, n);
        if (out && !RAY_IS_ERR(out)) {
            double* d = (double*)ray_data(out);
            for (int64_t i = 0; i < n; i++) {
                int k = json_kind(it[i]);
                d[i] = k == JK_NULL ? NULL_F64 : k == JK_INT ? (double)it[i]->i64 : it[i]->f64;
            }
            out->len = n;
            if (any_null) out->attrs |= RAY_ATTR_HAS_NULLS;
        }
    } else {
        out = ray_vec_new(RAY_I64, n);
        if (out && !RAY_IS_ERR(out)) {
            int64_t* d = (int64_t*)ray_data(out);
            for (int64_t i = 0; i < n; i++) d[i] = json_kind(it[i]) == JK_NULL ? NULL_I64 : it[i]->i64;
            out->len = n;
            if (any_null) out->attrs |= RAY_ATTR_HAS_NULLS;
        }
    }
    ray_release(list);
    return out ? out : ray_error("oom", NULL);
}

static ray_t* jp_value(jp_t* jp, bool build);

/* The bytes of the blocks `v` itself holds (ray_shallow_bytes: a typed
 * vector with its string pool, an atom with its payload, a list's own
 * block), and a dict's keys and values blocks too.  Children of a list are
 * counted where they are built. */
static size_t jp_own_bytes(ray_t* v) {
    if (!v || RAY_IS_NULL(v) || RAY_IS_ERR(v)) return 0;
    size_t n = ray_shallow_bytes(v);          /* a STR vector's pool, an atom's payload included */
    if (v->type == RAY_DICT) {
        ray_t* k = ray_dict_keys(v);
        ray_t* vl = ray_dict_vals(v);
        n += (k ? ray_shallow_bytes(k) : 0) + (vl ? ray_shallow_bytes(vl) : 0);
    }
    return n;
}

/* Charge what `v`'s blocks really hold beyond the `pre` bytes charged for
 * them before they were built.  The value, or NULL (released) past the
 * cap. */
static ray_t* jp_settle(jp_t* jp, ray_t* v, size_t pre) {
    size_t own = jp_own_bytes(v);
    if (own > pre && !jp_bytes(jp, own - pre)) { ray_release(v); return NULL; }
    return v;
}

/* Polled once per this many members of one container, so that one
 * document of many members can be interrupted. */
#define JP_POLL 1024

static bool jp_poll(jp_t* jp, int64_t i) {
    if ((i & (JP_POLL - 1)) == 0 && i > 0 && pool_cancelled(NULL)) {
        jp_fail(jp, "cancel", "interrupted");
        return false;
    }
    return true;
}

/* The bytes a slot of a growing list may cost: its pointer, and as much
 * again for the doubling that grows it. */
#define JP_SLOT_BYTES (2 * sizeof(ray_t*))

/* An array being built: while its elements are one kind of scalar (and
 * nulls) they go straight into the typed vector they collapse to
 * (json_collapse's rule), so an array of a million numbers costs its
 * vector, not a million atoms; the first element that breaks the kind
 * turns the run into a LIST of atoms, and the build goes on in it. */
typedef struct {
    int     mode;        /* 0 nothing but nulls yet, 1 a typed run, 2 a LIST */
    int     kind;        /* JK_INT / JK_FLOAT / JK_BOOL / JK_STR of the run */
    int64_t n_null;      /* nulls so far (mode 0: all of them) */
    ray_t*  out;         /* the typed vector, or the LIST */
    ray_t*  str_nulls;   /* STR run: the positions that were null (I64) */
    ray_t*  ints;        /* F64 run holding integers: each cell's integer as
                          * written (NULL_I64 for a float or a null cell), so
                          * a run that breaks into a LIST keeps them exact */
    size_t  pre;         /* bytes charged for `out` before it is settled */
} jarr_t;

static void jarr_free(jarr_t* a) {
    if (a->out && !RAY_IS_ERR(a->out)) ray_release(a->out);
    if (a->str_nulls && !RAY_IS_ERR(a->str_nulls)) ray_release(a->str_nulls);
    if (a->ints && !RAY_IS_ERR(a->ints)) ray_release(a->ints);
    a->out = a->str_nulls = a->ints = NULL;
}

/* Turn the run into a LIST of the atoms its cells hold, each as it was
 * written (an integer of an F64 run its integer, a null cell an untyped
 * null).  The list is charged while the run still is, so the cap holds
 * over the copy, each atom by what it holds (a string with its bytes);
 * the run's charge is returned once it is freed. */
static bool jarr_to_list(jp_t* jp, jarr_t* a) {
    int64_t n = a->mode == 1 ? a->out->len : a->n_null;
    size_t run_pre = a->mode == 1 ? a->pre : 0;
    /* a slot per cell, an atom per cell that is not null */
    int64_t n_atoms = a->mode == 1 ? n : 0;
    if (a->mode == 1 && a->kind == JK_STR && a->str_nulls) n_atoms -= a->str_nulls->len;
    else if (a->mode == 1 && (a->kind == JK_INT || a->kind == JK_FLOAT)) {
        for (int64_t i = 0; i < n; i++) {
            bool nul = a->kind == JK_INT ? ((const int64_t*)ray_data(a->out))[i] == NULL_I64
                                         : ((const double*)ray_data(a->out))[i] != ((const double*)ray_data(a->out))[i];
            n_atoms -= nul;
        }
    }
    if (!jp_bytes(jp, (size_t)n * JP_SLOT_BYTES + (size_t)n_atoms * JP_ATOM_BYTES)) return false;
    const int64_t* iv = a->ints ? (const int64_t*)ray_data(a->ints) : NULL;
    ray_t* list = ray_list_new(n > 4 ? n : 4);
    if (!list || RAY_IS_ERR(list)) { jp_fail(jp, "oom", "out of memory"); return false; }
    const int64_t* sn = a->str_nulls ? (const int64_t*)ray_data(a->str_nulls) : NULL;
    int64_t sni = 0, nsn = a->str_nulls ? a->str_nulls->len : 0;
    for (int64_t i = 0; i < n && !RAY_IS_ERR(list); i++) {
        ray_t* v = RAY_NULL_OBJ;
        if (a->mode == 1) {
            switch (a->kind) {
            case JK_INT: { int64_t x = ((const int64_t*)ray_data(a->out))[i]; v = x == NULL_I64 ? RAY_NULL_OBJ : ray_i64(x); break; }
            case JK_FLOAT: {
                if (iv && iv[i] != NULL_I64) { v = ray_i64(iv[i]); break; }
                double x = ((const double*)ray_data(a->out))[i];
                v = x != x ? RAY_NULL_OBJ : ray_f64(x);
                break;
            }
            case JK_BOOL: v = ray_bool(((const uint8_t*)ray_data(a->out))[i] != 0); break;
            default: {
                if (sni < nsn && sn[sni] == i) { sni++; break; }
                size_t l = 0;
                const char* p = ray_str_vec_get(a->out, i, &l);
                v = ray_str(p ? p : "", p ? l : 0);
            }
            }
        }
        if (!v || RAY_IS_ERR(v)) { ray_release(list); jp->err = v ? v : ray_error("oom", NULL); return false; }
        /* an atom is charged what it holds: a string's bytes beyond the
         * estimate charged for it above */
        size_t own = RAY_IS_NULL(v) ? 0 : ray_shallow_bytes(v);
        if (own > JP_ATOM_BYTES && !jp_bytes(jp, own - JP_ATOM_BYTES)) {
            ray_release(v); ray_release(list); return false;
        }
        list = ray_list_append(list, v);
        ray_release(v);
    }
    if (RAY_IS_ERR(list)) { jp->err = list; return false; }
    jarr_free(a);
    jp->bytes -= run_pre < jp->bytes ? run_pre : jp->bytes;
    a->out = list;
    a->mode = 2;
    a->pre = (size_t)n * JP_SLOT_BYTES;
    return true;
}

/* Start a typed run of `kind` holding the nulls seen so far. */
static bool jarr_start(jp_t* jp, jarr_t* a, int kind) {
    int8_t t = kind == JK_INT ? RAY_I64 : kind == JK_FLOAT ? RAY_F64 : kind == JK_BOOL ? RAY_BOOL : RAY_STR;
    ray_t* v = ray_vec_new(t, a->n_null + 8);
    if (!v || RAY_IS_ERR(v)) { jp_fail(jp, "oom", "out of memory"); return false; }
    a->out = v;
    a->kind = kind;
    a->mode = 1;
    for (int64_t i = 0; i < a->n_null; i++) {
        if (kind == JK_INT)   { int64_t x = NULL_I64; v = ray_vec_append(v, &x); }
        else if (kind == JK_FLOAT) { double x = NULL_F64; v = ray_vec_append(v, &x); }
        else {
            v = ray_str_vec_append(v, "", 0);
            if (!RAY_IS_ERR(v)) {
                if (!a->str_nulls) a->str_nulls = ray_vec_new(RAY_I64, 8);
                a->str_nulls = ray_vec_append(a->str_nulls, &i);
            }
        }
        if (RAY_IS_ERR(v) || RAY_IS_ERR(a->str_nulls)) { a->out = NULL; jp->err = ray_error("oom", NULL); return false; }
        a->out = v;
    }
    return true;
}

/* Add one built element `v` (consumed).  `built` is what building it was
 * charged: in a typed run that charge is returned and the cell's charged
 * instead. */
static bool jarr_add(jp_t* jp, jarr_t* a, ray_t* v, size_t built) {
    int k = json_kind(v);
    if (a->mode == 0) {
        if (k == JK_NULL) { a->n_null++; return true; }
        bool scalar = k == JK_INT || k == JK_FLOAT || k == JK_STR || (k == JK_BOOL && a->n_null == 0);
        if (scalar ? !jarr_start(jp, a, k) : !jarr_to_list(jp, a)) { ray_release(v); return false; }
    } else if (a->mode == 1) {
        bool fits = k == a->kind || (k == JK_NULL && a->kind != JK_BOOL) ||
                    (a->kind == JK_FLOAT && k == JK_INT) || (a->kind == JK_INT && k == JK_FLOAT);
        if (fits && a->kind == JK_INT && k == JK_FLOAT) {
            /* integers then a float: the run becomes F64, its integers
             * kept as written in `ints` */
            if (!jp_bytes(jp, (size_t)a->out->len * 2 * sizeof(double))) { ray_release(v); return false; }
            a->pre += (size_t)a->out->len * 2 * sizeof(double);
            ray_t* f = ray_vec_new(RAY_F64, a->out->len + 8);
            if (!f || RAY_IS_ERR(f)) { ray_release(v); jp_fail(jp, "oom", "out of memory"); return false; }
            const int64_t* d = (const int64_t*)ray_data(a->out);
            for (int64_t i = 0; i < a->out->len && !RAY_IS_ERR(f); i++) {
                double x = d[i] == NULL_I64 ? NULL_F64 : (double)d[i];
                f = ray_vec_append(f, &x);
            }
            if (RAY_IS_ERR(f)) { ray_release(v); jp->err = f; return false; }
            a->ints = a->out;
            a->out = f;
            a->kind = JK_FLOAT;
        } else if (fits && a->kind == JK_FLOAT && k == JK_INT && !a->ints) {
            /* floats then an integer: start keeping the integers */
            if (!jp_bytes(jp, (size_t)a->out->len * 2 * sizeof(int64_t))) { ray_release(v); return false; }
            a->pre += (size_t)a->out->len * 2 * sizeof(int64_t);
            ray_t* iv = ray_vec_new(RAY_I64, a->out->len + 8);
            for (int64_t i = 0; iv && !RAY_IS_ERR(iv) && i < a->out->len; i++) {
                int64_t x = NULL_I64;
                iv = ray_vec_append(iv, &x);
            }
            if (!iv || RAY_IS_ERR(iv)) { ray_release(v); jp->err = iv ? iv : ray_error("oom", NULL); return false; }
            a->ints = iv;
        }
        if (!fits && !jarr_to_list(jp, a)) { ray_release(v); return false; }
    }
    if (a->mode == 2) {
        a->pre += JP_SLOT_BYTES;
        if (!jp_bytes(jp, JP_SLOT_BYTES)) { ray_release(v); return false; }
        a->out = ray_list_append(a->out, v);
        ray_release(v);
        if (RAY_IS_ERR(a->out)) { jp->err = a->out; a->out = NULL; return false; }
        return true;
    }
    /* a typed run: the atom's charge back, the cell's (with its growth) in */
    jp->bytes -= built < jp->bytes ? built : jp->bytes;
    /* the cell and its pooled bytes, each doubled for the growth that
     * copies them; an F64 run keeping integers, that cell too */
    size_t cell = a->kind == JK_STR ? 2 * 16 + (k == JK_STR ? 2 * ray_str_len(v) : 0)
                                    : 2 * 8 * (a->ints ? 2 : 1);
    a->pre += cell;
    if (!jp_bytes(jp, cell)) { ray_release(v); return false; }
    ray_t* o = a->out;
    if (k == JK_NULL) {
        if (a->kind == JK_INT) { int64_t x = NULL_I64; o = ray_vec_append(o, &x); }
        else if (a->kind == JK_FLOAT) {
            double x = NULL_F64;
            o = ray_vec_append(o, &x);
            if (a->ints) {
                int64_t xi = NULL_I64;
                a->ints = ray_vec_append(a->ints, &xi);
                if (RAY_IS_ERR(a->ints)) { jp->err = a->ints; a->ints = NULL; }
            }
        }
        else {
            int64_t at = o->len;
            o = ray_str_vec_append(o, "", 0);
            if (!a->str_nulls) a->str_nulls = ray_vec_new(RAY_I64, 8);
            if (a->str_nulls && !RAY_IS_ERR(a->str_nulls)) a->str_nulls = ray_vec_append(a->str_nulls, &at);
            if (!a->str_nulls || RAY_IS_ERR(a->str_nulls)) { a->str_nulls = NULL; jp->err = ray_error("oom", NULL); }
        }
    } else if (a->kind == JK_INT) { o = ray_vec_append(o, &v->i64); }
    else if (a->kind == JK_FLOAT) {
        double x = k == JK_INT ? (double)v->i64 : v->f64;
        o = ray_vec_append(o, &x);
        if (a->ints) {
            int64_t xi = k == JK_INT ? v->i64 : NULL_I64;
            a->ints = ray_vec_append(a->ints, &xi);
            if (RAY_IS_ERR(a->ints)) { jp->err = a->ints; a->ints = NULL; }
        }
    }
    else if (a->kind == JK_BOOL) { uint8_t x = v->b8 ? 1 : 0; o = ray_vec_append(o, &x); }
    else o = ray_str_vec_append(o, ray_str_ptr(v), ray_str_len(v));
    ray_release(v);
    if (RAY_IS_ERR(o)) { a->out = NULL; jp->err = o; return false; }
    a->out = o;
    return !jp->err;
}

/* The finished array: the typed run (its nulls flagged), the LIST, or a
 * LIST of the nulls when nothing else came. */
static ray_t* jarr_finish(jp_t* jp, jarr_t* a) {
    if (a->mode == 0 && !jarr_to_list(jp, a)) { jarr_free(a); return NULL; }
    ray_t* out = a->out;
    a->out = NULL;
    if (a->mode == 1 && (a->kind == JK_INT || a->kind == JK_FLOAT)) {
        for (int64_t i = 0; i < out->len; i++) {
            bool nul = a->kind == JK_INT ? ((const int64_t*)ray_data(out))[i] == NULL_I64
                                         : ((const double*)ray_data(out))[i] != ((const double*)ray_data(out))[i];
            if (nul) { out->attrs |= RAY_ATTR_HAS_NULLS; break; }
        }
    }
    if (a->mode == 1 && a->kind == JK_STR && a->str_nulls) out->attrs |= RAY_ATTR_HAS_NULLS;
    if (a->str_nulls) { ray_release(a->str_nulls); a->str_nulls = NULL; }
    if (a->ints) { ray_release(a->ints); a->ints = NULL; }
    return jp_settle(jp, out, a->pre);
}

static ray_t* jp_array(jp_t* jp, bool build) {
    jp->p++;                                   /* '[' */
    jarr_t a = { 0, 0, 0, NULL, NULL, NULL, 0 };
    jp_ws(jp);
    if (jp->p < jp->end && *jp->p == ']') {
        jp->p++;
        return build ? jarr_finish(jp, &a) : RAY_NULL_OBJ;
    }
    for (int64_t i = 0;; i++) {
        if (!jp_poll(jp, i)) { jarr_free(&a); return NULL; }
        size_t before = jp->bytes;
        ray_t* v = jp_value(jp, build);
        if (!v) { jarr_free(&a); return NULL; }
        if (build && !jarr_add(jp, &a, v, jp->bytes - before)) { jarr_free(&a); return NULL; }
        jp_ws(jp);
        if (jp->p < jp->end && *jp->p == ',') { jp->p++; jp_ws(jp); continue; }
        if (jp->p < jp->end && *jp->p == ']') { jp->p++; break; }
        jarr_free(&a);
        return jp_unexpected(jp, "',' or ']'");
    }
    return build ? jarr_finish(jp, &a) : RAY_NULL_OBJ;
}

/* One key of the duplicate pass: its bytes, and its first 8 bytes as a
 * big-endian word so most comparisons are one integer compare. */
typedef struct { const char* p; uint32_t l; uint64_t pre; } jkey_t;

static inline int jp_key_cmp(const jkey_t* ks, int64_t i, int64_t j) {
    const jkey_t* a = &ks[i];
    const jkey_t* b = &ks[j];
    if (a->pre != b->pre) return a->pre < b->pre ? -1 : 1;
    size_t m = a->l < b->l ? a->l : b->l;
    int c = m > 8 ? memcmp(a->p + 8, b->p + 8, m - 8) : 0;
    if (c) return c;
    if (a->l != b->l) return a->l < b->l ? -1 : 1;
    return i < j ? -1 : i > j;
}

/* A repeated key keeps its first position and its last value.  The keys
 * are ordered by a merge sort on their bytes, O(n log n) whatever they
 * are: no hash, so no input can be crafted to make the pass slow.  On
 * success *keysp / *valsp are the deduplicated pair (the originals when no
 * key repeats).  The pair is built beside the originals, so it is charged
 * before it is built; once the originals and the pass's buffer are freed
 * their charges are returned, and *prep (what the object has been charged)
 * becomes the new pair's. */
static bool jp_dedup(jp_t* jp, ray_t** keysp, ray_t** valsp, size_t* prep) {
    ray_t* keys = *keysp;
    ray_t* vals = *valsp;
    int64_t n = keys->len;
    if (n < 2) return true;
    size_t need = (size_t)n * (2 * sizeof(int64_t) + sizeof(jkey_t));
    if (!jp_bytes(jp, need)) return false;
    int64_t* idx = (int64_t*)ray_alloc_raw(need);
    if (!idx) { jp_fail(jp, "oom", "out of memory"); return false; }
    int64_t* tmp = idx + n;
    jkey_t* ks = (jkey_t*)(tmp + n);
    for (int64_t i = 0; i < n; i++) {
        size_t l = 0;
        const char* p = ray_str_vec_get(keys, i, &l);
        uint64_t pre = 0;
        for (size_t k = 0; k < 8; k++) pre = (pre << 8) | (k < l ? (uint8_t)p[k] : 0);
        ks[i] = (jkey_t){ p ? p : "", (uint32_t)l, pre };
        idx[i] = i;
    }
    for (int64_t w = 1; w < n; w *= 2) {
        if (pool_cancelled(NULL)) { ray_free_raw(idx); jp_fail(jp, "cancel", "interrupted"); return false; }
        for (int64_t lo = 0; lo < n; lo += 2 * w) {
            int64_t mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;
            int64_t a = lo, b = mid, o = lo;
            while (a < mid && b < hi) tmp[o++] = jp_key_cmp(ks, idx[a], idx[b]) <= 0 ? idx[a++] : idx[b++];
            while (a < mid) tmp[o++] = idx[a++];
            while (b < hi) tmp[o++] = idx[b++];
        }
        int64_t* t = idx; idx = tmp; tmp = t;
    }
    int64_t* base = idx < tmp ? idx : tmp;       /* the block to free */
    /* tmp[i]: the position whose value position i keeps, -1 when dropped */
    bool dup = false;
    for (int64_t i = 0; i < n; i++) tmp[i] = -1;
    for (int64_t a = 0; a < n; ) {
        int64_t b = a + 1;
        const jkey_t* ka = &ks[idx[a]];
        for (; b < n; b++) {
            const jkey_t* kb = &ks[idx[b]];
            if (kb->l != ka->l || kb->pre != ka->pre ||
                (ka->l > 8 && memcmp(ka->p + 8, kb->p + 8, ka->l - 8) != 0)) break;
        }
        tmp[idx[a]] = idx[b - 1];
        dup |= b - a > 1;
        a = b;
    }
    if (!dup) { ray_free_raw(base); jp->bytes -= need < jp->bytes ? need : jp->bytes; return true; }
    /* The pair is this parse's alone (it always is: jp_object built it):
     * compact it in place, so no second pair exists and nothing more is
     * charged.  A kept key's cell moves down over a dropped one (its pooled
     * bytes stay where they are), and the values a repeated key replaced
     * are released.  Position i keeps value tmp[i] >= i, and a move only
     * writes below i, so no value is read after it was overwritten. */
    if (ray_atomic_load(&keys->rc) == 1 && ray_atomic_load(&vals->rc) == 1 &&
        !(keys->attrs & RAY_ATTR_SLICE) && !(vals->attrs & RAY_ATTR_SLICE) &&
        keys->type == RAY_STR && vals->type == RAY_LIST) {
        ray_t** items = (ray_t**)ray_data(vals);
        ray_str_t* cells = (ray_str_t*)ray_data(keys);
        uint8_t* used = (uint8_t*)idx;              /* idx is no longer needed */
        memset(used, 0, (size_t)n);
        for (int64_t i = 0; i < n; i++) if (tmp[i] >= 0) used[tmp[i]] = 1;
        for (int64_t i = 0; i < n; i++)
            if (!used[i]) { ray_release(items[i]); items[i] = NULL; }
        int64_t j = 0;
        for (int64_t i = 0; i < n; i++) {
            if (tmp[i] < 0) continue;
            cells[j] = cells[i];
            items[j] = items[tmp[i]];
            j++;
        }
        keys->len = j;
        vals->len = j;
        ray_free_raw(base);
        jp->bytes -= need < jp->bytes ? need : jp->bytes;
        return true;
    }
    /* the new pair: room for n key cells and n value slots (both made at
     * n), and the kept keys' pooled bytes, doubled for the pool's growth
     * as for the keys read */
    size_t kept_bytes = 0;
    for (int64_t i = 0; i < n; i++)
        if (tmp[i] >= 0 && ks[i].l > RAY_STR_INLINE_MAX) kept_bytes += ks[i].l;
    size_t built = (size_t)n * (16 + sizeof(ray_t*)) + 2 * kept_bytes;
    if (!jp_bytes(jp, built)) { ray_free_raw(base); return false; }
    ray_t* nk = ray_vec_new(RAY_STR, n);
    ray_t* nv = ray_list_new(n);
    ray_t** items = (ray_t**)ray_data(vals);
    for (int64_t i = 0; i < n && nk && !RAY_IS_ERR(nk) && nv && !RAY_IS_ERR(nv); i++) {
        if (tmp[i] < 0) continue;
        nk = ray_str_vec_append(nk, ks[i].p, ks[i].l);
        if (!RAY_IS_ERR(nk)) nv = ray_list_append(nv, items[tmp[i]]);
    }
    ray_free_raw(base);
    if (!nk || RAY_IS_ERR(nk) || !nv || RAY_IS_ERR(nv)) {
        if (nk && !RAY_IS_ERR(nk)) ray_release(nk);
        if (nv && !RAY_IS_ERR(nv)) ray_release(nv);
        jp_fail(jp, "oom", "out of memory");
        return false;
    }
    ray_release(keys);
    ray_release(vals);
    *keysp = nk;
    *valsp = nv;
    /* the buffer and the originals are gone: their charges back */
    size_t back = need + *prep;
    jp->bytes -= back < jp->bytes ? back : jp->bytes;
    *prep = built;
    return true;
}

static ray_t* jp_object(jp_t* jp, bool build) {
    jp->p++;                                   /* '{' */
    ray_t* keys = NULL;
    ray_t* vals = NULL;
    size_t pre = 0;
    if (build) {
        keys = ray_vec_new(RAY_STR, 4);
        vals = ray_list_new(4);
        if (!keys || RAY_IS_ERR(keys) || !vals || RAY_IS_ERR(vals)) {
            if (keys && !RAY_IS_ERR(keys)) ray_release(keys);
            if (vals && !RAY_IS_ERR(vals)) ray_release(vals);
            return jp_fail(jp, "oom", "out of memory");
        }
    }
    ray_t* out = NULL;
    jp_ws(jp);
    if (jp->p < jp->end && *jp->p == '}') { jp->p++; goto done; }
    for (int64_t i = 0;; i++) {
        if (!jp_poll(jp, i)) goto fail;
        if (jp->p >= jp->end || *jp->p != '"') { jp_unexpected(jp, "a string key"); goto fail; }
        const char* kp; size_t kl;
        if (!jp_string(jp, &kp, &kl)) goto fail;
        if (build) {
            /* the key's cell and its pooled bytes, each doubled for the
             * growth that copies them (a cell of a string run, jarr_add),
             * and a slot of the values list.  A key of up to
             * RAY_STR_INLINE_MAX bytes lives in its cell: no pooled bytes. */
            size_t c = 2 * 16 + (kl > RAY_STR_INLINE_MAX ? 2 * kl : 0) + JP_SLOT_BYTES;
            if (!jp_bytes(jp, c)) goto fail;
            pre += c;
            keys = ray_str_vec_append(keys, kp, kl);
            if (RAY_IS_ERR(keys)) { jp->err = keys; keys = NULL; goto fail; }
        }
        jp_ws(jp);
        if (jp->p >= jp->end || *jp->p != ':') { jp_unexpected(jp, "':'"); goto fail; }
        jp->p++;
        jp_ws(jp);
        ray_t* v = jp_value(jp, build);
        if (!v) goto fail;
        if (build) {
            vals = ray_list_append(vals, v);
            ray_release(v);
            if (RAY_IS_ERR(vals)) { jp->err = vals; vals = NULL; goto fail; }
        }
        jp_ws(jp);
        if (jp->p < jp->end && *jp->p == ',') { jp->p++; jp_ws(jp); continue; }
        if (jp->p < jp->end && *jp->p == '}') { jp->p++; break; }
        jp_unexpected(jp, "',' or '}'");
        goto fail;
    }
done:
    if (!build) return RAY_NULL_OBJ;
    if (!jp_dedup(jp, &keys, &vals, &pre)) goto fail;
    out = ray_dict_new(keys, vals);
    if (RAY_IS_ERR(out)) { jp->err = out; return NULL; }
    return jp_settle(jp, out, pre);
fail:
    if (keys) ray_release(keys);
    if (vals) ray_release(vals);
    return NULL;
}

/* A built scalar, charged: the node and the bytes it was estimated at
 * before it was built (`est`, already charged), settled to its real size. */
static ray_t* jp_scalar(jp_t* jp, ray_t* v, size_t est) {
    if (!v || RAY_IS_ERR(v)) { jp->err = v ? v : ray_error("oom", NULL); return NULL; }
    return jp_settle(jp, v, est);
}

/* One value at jp->p (whitespace before it skipped by the caller).  Owned
 * value, RAY_NULL_OBJ when `build` is false, NULL with jp->err set on a
 * failure.  A value that is only validated (`build` false) is charged its
 * depth, not nodes or bytes. */
static ray_t* jp_value(jp_t* jp, bool build) {
    if (jp->p >= jp->end) return jp_unexpected(jp, "a value");
    switch (*jp->p) {
    case '{': case '[': {
        if (++jp->depth > jp->max_depth) return jp_fail(jp, "limit", "depth limit of %zu exceeded", jp->max_depth);
        if (ray_stack_short()) return jp_fail(jp, "limit", "nesting too deep for this thread's stack");
        if (build && !jp_charge(jp, 0)) return NULL;
        ray_t* v = *jp->p == '{' ? jp_object(jp, build) : jp_array(jp, build);
        jp->depth--;
        return v;
    }
    case '"': {
        const char* sp; size_t sl;
        if (!jp_string(jp, &sp, &sl)) return NULL;
        if (!build) return RAY_NULL_OBJ;
        if (!jp_charge(jp, JP_ATOM_BYTES + sl)) return NULL;
        return jp_scalar(jp, ray_str(sp, sl), JP_ATOM_BYTES + sl);
    }
    case 't': case 'f': {
        bool t = *jp->p == 't';
        if (!jp_literal(jp, t ? "true" : "false", t ? 4 : 5)) return NULL;
        if (!build) return RAY_NULL_OBJ;
        if (!jp_charge(jp, JP_ATOM_BYTES)) return NULL;
        return jp_scalar(jp, ray_bool(t), JP_ATOM_BYTES);
    }
    case 'n':
        if (!jp_literal(jp, "null", 4)) return NULL;
        if (build && !jp_charge(jp, 0)) return NULL;
        return RAY_NULL_OBJ;
    default:
        if (*jp->p == '-' || (*jp->p >= '0' && *jp->p <= '9')) return jp_number(jp, build);
        return jp_unexpected(jp, "a value");
    }
}

static void jp_init(jp_t* jp, const char* who, const ray_read_limits_t* lim) {
    memset(jp, 0, sizeof *jp);
    jp->who = who;
    jp->row = -1;
    jp->max_depth = lim->max_depth < RAY_READ_DEPTH_CAP ? lim->max_depth : RAY_READ_DEPTH_CAP;
    jp->max_nodes = lim->max_nodes;
    jp->max_alloc = lim->max_alloc;
}

/* Point the reader at one document and reset its counters. */
static bool jp_doc(jp_t* jp, const char* s, size_t len, int64_t row, const ray_read_limits_t* lim) {
    jp->s = jp->p = (const uint8_t*)s;
    jp->end = jp->s + len;
    jp->row = row;
    jp->depth = jp->nodes = 0;
    jp->bytes = jp->buf_cap;                   /* the decoded-string buffer it keeps */
    if (len > lim->max_input) {
        if (!jp->err)
            jp->err = row >= 0
                ? ray_error("limit", "%s: input limit of %zu bytes exceeded by %s %lld (%zu bytes)", jp->who, lim->max_input,
                            jp->unit ? jp->unit : "element", (long long)row, len)
                : ray_error("limit", "%s: input limit of %zu bytes exceeded (%zu bytes)", jp->who, lim->max_input, len);
        return false;
    }
    return true;
}

/* After the top-level value: only whitespace. */
static bool jp_end(jp_t* jp) {
    jp_ws(jp);
    if (jp->p < jp->end) { jp_unexpected(jp, "the end of input after the value"); return false; }
    return true;
}

static ray_t* jp_parse_doc(jp_t* jp, const char* s, size_t len, int64_t row, const ray_read_limits_t* lim) {
    /* "" is the STR null: a null document reads as null */
    if (len == 0) return RAY_NULL_OBJ;
    if (!jp_doc(jp, s, len, row, lim)) return NULL;
    jp_ws(jp);
    ray_t* v = jp_value(jp, true);
    if (!v) return NULL;
    if (!jp_end(jp)) { ray_release(v); return NULL; }
    return v;
}

static ray_t* jp_finish(jp_t* jp, ray_t* v) {
    if (jp->buf) ray_free_raw(jp->buf);
    jp->buf = NULL;
    if (!v) return jp->err ? jp->err : ray_error("oom", NULL);
    if (jp->err) { ray_release(jp->err); }
    return v;
}

/* The documents of a string collection: a STR vector, a list of strings (a
 * null among them is a null document), or a parted STR column.  Calls
 * `each` per document in row order; stops at the first NULL it returns. */
typedef ray_t* (*jp_each_fn)(jp_t* jp, const char* s, size_t l, int64_t row, void* ctx);

static bool jp_is_docs(ray_t* x) {
    if (x->type == RAY_STR || x->type == RAY_LIST) return true;
    return RAY_IS_PARTED(x->type) && RAY_PARTED_BASETYPE(x->type) == RAY_STR;
}

static ray_t* jp_docs(jp_t* jp, ray_t* x, jp_each_fn each, void* ctx) {
    bool parted = RAY_IS_PARTED(x->type);
    int64_t nseg = parted ? x->len : 1;
    ray_t** segs = parted ? (ray_t**)ray_data(x) : &x;
    int64_t row = 0;
    for (int64_t sg = 0; sg < nseg; sg++) {
        ray_t* v = segs[sg];
        if (!v || RAY_IS_ERR(v)) continue;
        ray_t** items = v->type == RAY_LIST ? (ray_t**)ray_data(v) : NULL;
        for (int64_t i = 0; i < v->len; i++, row++) {
            if ((row & (RAY_MORSEL_ELEMS - 1)) == 0 && pool_cancelled(NULL)) return ray_error("cancel", NULL);
            const char* s = "";
            size_t l = 0;
            if (items) {
                ray_t* it = items[i];
                if (it && it->type == -RAY_STR) { s = ray_str_ptr(it); l = ray_str_len(it); }
                else if (!it || !RAY_IS_NULL(it))
                    return ray_error("type", "%s: list items must be strings, got %s at element %lld",
                                     jp->who, it ? ray_type_name(it->type) : "null", (long long)row);
            } else {
                s = ray_str_vec_get(v, i, &l);
                if (!s) { s = ""; l = 0; }
            }
            ray_t* r = each(jp, s, l, row, ctx);
            if (!r) return jp->err ? jp->err : ray_error("oom", NULL);
        }
    }
    return NULL;
}

typedef struct { ray_t* out; const ray_read_limits_t* lim; } jp_parse_ctx_t;

static ray_t* jp_parse_each(jp_t* jp, const char* s, size_t l, int64_t row, void* vctx) {
    jp_parse_ctx_t* c = (jp_parse_ctx_t*)vctx;
    ray_t* v = jp_parse_doc(jp, s, l, row, c->lim);
    if (!v) return NULL;
    c->out = ray_list_append(c->out, v);
    ray_release(v);
    if (RAY_IS_ERR(c->out)) { jp->err = c->out; c->out = NULL; return NULL; }
    return c->out;
}

static ray_t* jp_input(ray_t* x) {
    if (x && !RAY_IS_ERR(x) && ray_is_lazy(x)) return ray_lazy_materialize(x);
    return x;
}

ray_t* ray_json_parse_fn(ray_t** args, int64_t n) {
    if (n < 1 || n > 2)
        return ray_error("arity", ".json.parse: expects text and optional limits, got %lld arguments", (long long)n);
    ray_read_limits_t lim = ray_read_limits_default();
    if (n == 2) {
        ray_t* err = ray_read_limits_apply(".json.parse", args[1], &lim);
        if (err) return err;
    }
    ray_t* x = jp_input(args[0]);
    if (!x || RAY_IS_ERR(x)) return x;
    jp_t jp;
    jp_init(&jp, ".json.parse", &lim);
    if (x->type == -RAY_STR)
        return jp_finish(&jp, jp_parse_doc(&jp, ray_str_ptr(x), ray_str_len(x), -1, &lim));
    if (!jp_is_docs(x))
        return ray_error("type", ".json.parse: expected a string or a string vector, got %s", ray_type_name(x->type));
    jp_parse_ctx_t c = { ray_list_new(x->len > 0 ? x->len : 1), &lim };
    if (!c.out || RAY_IS_ERR(c.out)) return c.out ? c.out : ray_error("oom", NULL);
    ray_t* err = jp_docs(&jp, x, jp_parse_each, &c);
    if (jp.buf) ray_free_raw(jp.buf);
    if (err) {
        if (c.out) ray_release(c.out);
        return err;
    }
    return json_collapse(c.out, false);
}

/* ---- (.json.get s path) ------------------------------------------------- */

typedef struct { const char* p; size_t l; int64_t idx; } jseg_t;   /* idx -1: not an index */

#define JSON_PATH_MAX 64

/* Split the path p[0..l) at its dots.  A segment of digits (no leading
 * zero) is also an array index. */
static ray_t* json_path_bytes(const char* p, size_t l, jseg_t* segs, int* nseg, int64_t row, const char* unit) {
    char of[48] = "";
    /* "path of element 3" for one path per document, "path 3" for one
     * document read by a vector of paths */
    if (row >= 0 && unit) snprintf(of, sizeof of, " %lld", (long long)row);
    else if (row >= 0) snprintf(of, sizeof of, " of element %lld", (long long)row);
    if (l == 0) return ray_error("domain", ".json.get: path%s is empty", of);
    int k = 0;
    size_t st = 0;
    for (size_t i = 0; i <= l; i++) {
        if (i < l && p[i] != '.') continue;
        if (i == st) return ray_error("domain", ".json.get: empty segment in path%s at byte %zu", of, i);
        if (k == JSON_PATH_MAX) return ray_error("limit", ".json.get: path%s has more than %d segments", of, JSON_PATH_MAX);
        jseg_t* sg = &segs[k++];
        sg->p = p + st;
        sg->l = i - st;
        sg->idx = -1;
        if (sg->l <= 18 && (sg->l == 1 || sg->p[0] != '0')) {
            int64_t v = 0;
            size_t j = 0;
            for (; j < sg->l && sg->p[j] >= '0' && sg->p[j] <= '9'; j++) v = v * 10 + (sg->p[j] - '0');
            if (j == sg->l) sg->idx = v;
        }
        st = i + 1;
    }
    *nseg = k;
    return NULL;
}

/* The path argument as one path: a string or a symbol. */
static ray_t* json_path(ray_t* path, jseg_t* segs, int* nseg) {
    if (path && path->type == -RAY_STR) return json_path_bytes(ray_str_ptr(path), ray_str_len(path), segs, nseg, -1, NULL);
    if (path && path->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(path->i64);
        return json_path_bytes(s ? ray_str_ptr(s) : "", s ? ray_str_len(s) : 0, segs, nseg, -1, NULL);
    }
    return ray_error("type", ".json.get: path must be a string or a string vector, got %s",
                     path ? ray_type_name(path->type) : "null");
}

/* The value at segs[k..] of the value at jp->p, built; RAY_NULL_OBJ when the
 * path is missing; NULL on a malformed document.  The rest of the value is
 * validated without being built.  In an object the last of a repeated key
 * is the one taken, as .json.parse keeps it. */
static ray_t* jp_nav(jp_t* jp, const jseg_t* segs, int nseg, int k) {
    if (k == nseg) return jp_value(jp, true);
    if (jp->p >= jp->end) return jp_unexpected(jp, "a value");
    uint8_t c = *jp->p;
    if (c != '{' && c != '[') {
        ray_t* v = jp_value(jp, false);
        return v ? RAY_NULL_OBJ : NULL;
    }
    if (++jp->depth > jp->max_depth) return jp_fail(jp, "limit", "depth limit of %zu exceeded", jp->max_depth);
    if (ray_stack_short()) return jp_fail(jp, "limit", "nesting too deep for this thread's stack");
    jp->p++;
    ray_t* found = RAY_NULL_OBJ;
    jp_ws(jp);
    if (c == '{') {
        if (jp->p < jp->end && *jp->p == '}') { jp->p++; goto done; }
        for (int64_t i = 0;; i++) {
            if (!jp_poll(jp, i)) goto fail;
            if (jp->p >= jp->end || *jp->p != '"') { jp_unexpected(jp, "a string key"); goto fail; }
            const char* kp; size_t kl;
            if (!jp_string(jp, &kp, &kl)) goto fail;
            bool hit = kl == segs[k].l && memcmp(kp, segs[k].p, kl) == 0;
            jp_ws(jp);
            if (jp->p >= jp->end || *jp->p != ':') { jp_unexpected(jp, "':'"); goto fail; }
            jp->p++;
            jp_ws(jp);
            ray_t* v = hit ? jp_nav(jp, segs, nseg, k + 1) : jp_value(jp, false);
            if (!v) goto fail;
            if (hit) { ray_release(found); found = v; }
            jp_ws(jp);
            if (jp->p < jp->end && *jp->p == ',') { jp->p++; jp_ws(jp); continue; }
            if (jp->p < jp->end && *jp->p == '}') { jp->p++; break; }
            jp_unexpected(jp, "',' or '}'");
            goto fail;
        }
    } else {
        if (jp->p < jp->end && *jp->p == ']') { jp->p++; goto done; }
        for (int64_t i = 0;; i++) {
            if (!jp_poll(jp, i)) goto fail;
            bool hit = segs[k].idx == i;
            ray_t* v = hit ? jp_nav(jp, segs, nseg, k + 1) : jp_value(jp, false);
            if (!v) goto fail;
            if (hit) { ray_release(found); found = v; }
            jp_ws(jp);
            if (jp->p < jp->end && *jp->p == ',') { jp->p++; jp_ws(jp); continue; }
            if (jp->p < jp->end && *jp->p == ']') { jp->p++; break; }
            jp_unexpected(jp, "',' or ']'");
            goto fail;
        }
    }
done:
    jp->depth--;
    return found;
fail:
    ray_release(found);
    return NULL;
}

typedef struct {
    ray_t* out;
    const ray_read_limits_t* lim;
    const jseg_t* segs;
    int nseg;
    ray_t* paths;          /* a STR vector of one path per row, or NULL */
} jp_get_ctx_t;

static ray_t* jp_get_doc(jp_t* jp, const char* s, size_t l, int64_t row, jp_get_ctx_t* c) {
    jseg_t rsegs[JSON_PATH_MAX];
    const jseg_t* segs = c->segs;
    int nseg = c->nseg;
    if (c->paths) {
        size_t pl = 0;
        const char* pp = NULL;
        if (c->paths->type == RAY_SYM) {
            ray_t* ps = ray_sym_vec_cell(c->paths, row);
            if (ps) { pp = ray_str_ptr(ps); pl = ray_str_len(ps); }
        } else {
            pp = ray_str_vec_get(c->paths, row, &pl);
        }
        if (!pp || pl == 0) return RAY_NULL_OBJ;   /* a null path: no field */
        ray_t* perr = json_path_bytes(pp, pl, rsegs, &nseg, row, jp->unit);
        if (perr) { jp->err = perr; return NULL; }
        segs = rsegs;
    }
    if (l == 0) return RAY_NULL_OBJ;           /* a null document: no field */
    if (!jp_doc(jp, s, l, row, c->lim)) return NULL;
    jp_ws(jp);
    ray_t* v = jp_nav(jp, segs, nseg, 0);
    if (!v) return NULL;
    if (!jp_end(jp)) { ray_release(v); return NULL; }
    return v;
}

static ray_t* jp_get_each(jp_t* jp, const char* s, size_t l, int64_t row, void* vctx) {
    jp_get_ctx_t* c = (jp_get_ctx_t*)vctx;
    ray_t* v = jp_get_doc(jp, s, l, row, c);
    if (!v) return NULL;
    c->out = ray_list_append(c->out, v);
    ray_release(v);
    if (RAY_IS_ERR(c->out)) { jp->err = c->out; c->out = NULL; return NULL; }
    return c->out;
}

/* The type a typed .json.get asks for: RAY_STR, RAY_I64 or RAY_F64, or an
 * owned error.  B8 is refused: it has no null, and a missing field read
 * as false would pass `(not ...)` in a where: as a real false. */
static ray_t* json_get_type(ray_t* t, int8_t* out) {
    ray_t* nm = ray_sym_str(t->i64);
    const char* p = nm ? ray_str_ptr(nm) : "";
    size_t l = nm ? ray_str_len(nm) : 0;
    if (l == 3 && memcmp(p, "STR", 3) == 0) { *out = RAY_STR; return NULL; }
    if (l == 3 && memcmp(p, "I64", 3) == 0) { *out = RAY_I64; return NULL; }
    if (l == 3 && memcmp(p, "F64", 3) == 0) { *out = RAY_F64; return NULL; }
    if ((l == 2 && memcmp(p, "B8", 2) == 0) || (l == 4 && memcmp(p, "BOOL", 4) == 0))
        return ray_error("domain", ".json.get: 'B8 has no null to give a missing or other field; "
                         "use the untyped form and compare the field with true");
    return ray_error("domain", ".json.get: type must be 'STR, 'I64 or 'F64, got '%.*s", (int)l, p);
}

/* A found value as the asked type: the value when it is that kind (an
 * integer as an F64 for 'F64), else the type's null.  Consumes v. */
static ray_t* json_typed_atom(ray_t* v, int8_t t) {
    int k = json_kind(v);
    ray_t* out;
    if (t == RAY_STR) out = k == JK_STR ? (ray_retain(v), v) : ray_str("", 0);
    else if (t == RAY_I64) out = k == JK_INT ? ray_i64(v->i64) : ray_typed_null(-RAY_I64);
    else out = k == JK_FLOAT ? ray_f64(v->f64) : k == JK_INT ? ray_f64((double)v->i64) : ray_typed_null(-RAY_F64);
    if (v && !RAY_IS_NULL(v)) ray_release(v);
    return out ? out : ray_error("oom", NULL);
}

/* A LIST of found values (consumed) as a vector of the asked type, one
 * cell per value, the type's null where the value is another kind. */
static ray_t* json_typed_vec(ray_t* list, int8_t t) {
    int64_t n = list->len;
    ray_t** it = (ray_t**)ray_data(list);
    ray_t* out = ray_vec_new(t, n > 0 ? n : 1);
    bool any_null = false;
    for (int64_t i = 0; out && !RAY_IS_ERR(out) && i < n; i++) {
        int k = json_kind(it[i]);
        if (t == RAY_STR) {
            out = k == JK_STR ? ray_str_vec_append(out, ray_str_ptr(it[i]), ray_str_len(it[i]))
                              : ray_str_vec_append(out, "", 0);
        } else if (t == RAY_I64) {
            int64_t x = k == JK_INT ? it[i]->i64 : NULL_I64;
            any_null |= x == NULL_I64;
            out = ray_vec_append(out, &x);
        } else {
            double x = k == JK_FLOAT ? it[i]->f64 : k == JK_INT ? (double)it[i]->i64 : NULL_F64;
            any_null |= x != x;
            out = ray_vec_append(out, &x);
        }
    }
    ray_release(list);
    if (!out) return ray_error("oom", NULL);
    if (!RAY_IS_ERR(out) && any_null) out->attrs |= RAY_ATTR_HAS_NULLS;
    return out;
}

ray_t* ray_json_get_fn(ray_t** args, int64_t n) {
    if (n < 2 || n > 4)
        return ray_error("arity", ".json.get: expects text, a path, an optional type and optional limits, got %lld arguments", (long long)n);
    ray_read_limits_t lim = ray_read_limits_default();
    int8_t typed = 0;
    int64_t at = 2;
    if (n > at && args[at] && args[at]->type == -RAY_SYM) {
        ray_t* terr = json_get_type(args[at], &typed);
        if (terr) return terr;
        at++;
    }
    if (n > at + 1)
        return ray_error("arity", ".json.get: after the path come an optional type symbol and an optional limits dict");
    if (n > at) {
        ray_t* err = ray_read_limits_apply(".json.get", args[at], &lim);
        if (err) return err;
    }
    ray_t* path = jp_input(args[1]);
    if (!path || RAY_IS_ERR(path)) return path;
    jseg_t segs[JSON_PATH_MAX];
    int nseg = 0;
    /* one path per row: a STR or SYM vector */
    ray_t* paths = path->type == RAY_STR || path->type == RAY_SYM ? path : NULL;
    if (!paths) {
        ray_t* perr = json_path(path, segs, &nseg);
        if (perr) return perr;
    }
    ray_t* x = jp_input(args[0]);
    if (!x || RAY_IS_ERR(x)) return x;
    jp_t jp;
    jp_init(&jp, ".json.get", &lim);
    jp_get_ctx_t c = { NULL, &lim, segs, nseg, NULL };
    if (x->type == -RAY_STR && !paths) {
        ray_t* v = jp_finish(&jp, jp_get_doc(&jp, ray_str_ptr(x), ray_str_len(x), -1, &c));
        return typed && !RAY_IS_ERR(v) ? json_typed_atom(v, typed) : v;
    }
    if (x->type == -RAY_STR) {
        /* one document, a vector of paths: one value per path */
        c.paths = paths;
        jp.unit = "path";
        c.out = ray_list_new(paths->len > 0 ? paths->len : 1);
        if (!c.out || RAY_IS_ERR(c.out)) return c.out ? c.out : ray_error("oom", NULL);
        for (int64_t r = 0; r < paths->len; r++) {
            if (!jp_get_each(&jp, ray_str_ptr(x), ray_str_len(x), r, &c)) {
                if (jp.buf) ray_free_raw(jp.buf);
                if (c.out) ray_release(c.out);
                return jp.err ? jp.err : ray_error("oom", NULL);
            }
        }
        if (jp.buf) ray_free_raw(jp.buf);
        return typed ? json_typed_vec(c.out, typed) : json_collapse(c.out, true);
    }
    if (!jp_is_docs(x))
        return ray_error("type", ".json.get: expected a string or a string vector, got %s", ray_type_name(x->type));
    int64_t rows = RAY_IS_PARTED(x->type) ? ray_parted_nrows(x) : x->len;
    if (paths) {
        if (paths->len != rows)
            return ray_error("length", ".json.get: %lld paths for %lld documents", (long long)paths->len, (long long)rows);
        c.paths = paths;
    }
    c.out = ray_list_new(rows > 0 ? rows : 1);
    if (!c.out || RAY_IS_ERR(c.out)) return c.out ? c.out : ray_error("oom", NULL);
    ray_t* err = jp_docs(&jp, x, jp_get_each, &c);
    if (jp.buf) ray_free_raw(jp.buf);
    if (err) {
        if (c.out) ray_release(c.out);
        return err;
    }
    if (typed) return json_typed_vec(c.out, typed);
    /* no rows: what rows that all lack the field give, an empty STR */
    return json_collapse(c.out, true);
}

/* ==========================================================================
 * Writer
 * ========================================================================== */

typedef struct {
    char* p;
    size_t len, cap;
    ray_t* err;
    size_t depth;
    ray_dup_t dup;   /* what is written again for shared parts (#1627) */
} jw_t;

static bool jw_put(jw_t* w, const char* s, size_t n) {
    if (w->err) return false;
    if (n > w->cap - w->len) {
        size_t cap = w->cap ? w->cap : 256;
        while (cap - w->len < n) {
            if (cap > SIZE_MAX / 2) { w->err = ray_error("oom", NULL); return false; }
            cap *= 2;
        }
        char* np = (char*)ray_realloc_raw(w->p, cap);
        if (!np) { w->err = ray_error("oom", NULL); return false; }
        w->p = np;
        w->cap = cap;
    }
    memcpy(w->p + w->len, s, n);
    w->len += n;
    return true;
}
#define JW_LIT(w, s) jw_put(w, s, sizeof(s) - 1)

static bool jw_string(jw_t* w, const char* s, size_t n) {
    static const char hex[] = "0123456789abcdef";
    if (!JW_LIT(w, "\"")) return false;
    size_t run = 0;
    const uint8_t* u = (const uint8_t*)s;
    for (size_t i = 0; i < n; ) {
        uint8_t c = u[i];
        const char* e = NULL;
        char ub[6];
        size_t el = 2;
        switch (c) {
        case '"':  e = "\\\""; break;
        case '\\': e = "\\\\"; break;
        case '\b': e = "\\b";  break;
        case '\f': e = "\\f";  break;
        case '\n': e = "\\n";  break;
        case '\r': e = "\\r";  break;
        case '\t': e = "\\t";  break;
        default:
            if (c < 0x20) {
                ub[0] = '\\'; ub[1] = 'u'; ub[2] = '0'; ub[3] = '0';
                ub[4] = hex[c >> 4]; ub[5] = hex[c & 15];
                e = ub; el = 6;
            }
        }
        if (e) {
            if (!jw_put(w, s + i - run, run) || !jw_put(w, e, el)) return false;
            run = 0;
            i++;
            continue;
        }
        size_t k = utf8_seq(u + i, u + n);
        if (!k) {
            w->err = ray_error("domain", ".json.format: a string is not valid UTF-8 at byte %zu", i);
            return false;
        }
        run += k;
        i += k;
    }
    return jw_put(w, s + n - run, run) && JW_LIT(w, "\"");
}

static bool jw_i64(jw_t* w, int64_t v) {
    char b[24];
    int k = snprintf(b, sizeof b, "%lld", (long long)v);
    return jw_put(w, b, (size_t)k);
}

/* `d` printed with `fmt` ("%.*g" or "%.*f") at `prec` into b[0..cap),
 * NUL-terminated: its length, or -1 when snprintf fails or the result does
 * not fit.  The decimal point is '.' whatever the host's locale
 * (ray_snprintf_c), and an exponent is written short ("1e+05" as "1e5"). */
static int jw_fmt_c(char* b, size_t cap, const char* fmt, int prec, double d) {
    int k = ray_snprintf_c(b, cap, fmt, prec, d);
    if (k < 0 || (size_t)k >= cap) { b[0] = '\0'; return -1; }
    char* e = strchr(b, 'e');
    if (e) {
        char* q = e + 1;
        char* r = q;
        if (*r == '+') r++;
        else if (*r == '-') { q++; r++; }
        while (*r == '0' && r[1] != '\0') r++;
        memmove(q, r, strlen(r) + 1);
        k = (int)strlen(b);
    }
    return k;
}

/* Does b[0..k) read back as `d` (as an F32 when `f32`)? */
static bool jw_reads_back(const char* b, int k, double d, bool f32) {
    if (k <= 0) return false;
    char* end = NULL;
    double back = ray_strtod_c(b, &end);
    return end == b + k && (f32 ? (float)back == (float)d : back == d);
}

/* A candidate as JSON writes a float: with a '.' or an exponent, so that it
 * reads back as a float. */
static int jw_mark_float(char* b, int k, size_t cap) {
    for (int i = 0; i < k; i++) if (b[i] == '.' || b[i] == 'e') return k;
    if ((size_t)k + 3 > cap) return -1;
    b[k++] = '.'; b[k++] = '0'; b[k] = '\0';
    return k;
}

/* The shortest text that reads back as `d` (as an F32 when `f32`): the
 * fewest significant digits that read back, written as the shorter of the
 * %g form (an exponent once the digits run out) and the positional form,
 * the positional one on a tie.  A '.' or an exponent always, so that it
 * reads back as a float. */
static bool jw_float(jw_t* w, double d, bool f32) {
    if (d != d) return JW_LIT(w, "null");
    if (!isfinite(d)) { w->err = ray_error("domain", ".json.format: a float is not finite"); return false; }
    const int max_prec = f32 ? 9 : 17;
    char b[48];
    int prec = 1;
    int k = jw_fmt_c(b, sizeof b, "%.*g", prec, d);
    while (prec < max_prec && !jw_reads_back(b, k, d, f32))
        k = jw_fmt_c(b, sizeof b, "%.*g", ++prec, d);
    if (k > 0) k = jw_mark_float(b, k, sizeof b);
    if (k <= 0) {
        char t[32];
        ray_snprintf_c(t, sizeof t, "%g", d);
        w->err = ray_error("domain", ".json.format: cannot write the float %s", t);
        return false;
    }
    /* the positional form with the same significant digits */
    double ad = d < 0 ? -d : d;
    if (strchr(b, 'e') && ad >= 1e-6 && ad < 1e21) {
        int e10 = (int)floor(log10(ad));
        int dec = prec - 1 - e10 > 0 ? prec - 1 - e10 : 0;
        char c[48];
        int kc = jw_fmt_c(c, sizeof c, "%.*f", dec, d);
        if (jw_reads_back(c, kc, d, f32)) {
            kc = jw_mark_float(c, kc, sizeof c);
            if (kc > 0 && kc <= k) { memcpy(b, c, (size_t)kc + 1); k = kc; }
        }
    }
    return jw_put(w, b, (size_t)k);
}

static bool jw_f64(jw_t* w, double d) { return jw_float(w, d, false); }
static bool jw_f32(jw_t* w, float f)  { return jw_float(w, (double)f, true); }

/* A temporal or guid value as the string of its printed form. */
static bool jw_printed(jw_t* w, ray_t* atom) {
    if (!atom || RAY_IS_ERR(atom)) { w->err = atom ? atom : ray_error("oom", NULL); return false; }
    ray_t* s = ray_fmt(atom, RAY_FMT_COMPACT);
    ray_release(atom);
    if (!s || RAY_IS_ERR(s)) { w->err = s ? s : ray_error("oom", NULL); return false; }
    bool ok = jw_string(w, ray_str_ptr(s), ray_str_len(s));
    ray_release(s);
    return ok;
}

static bool jw_value(jw_t* w, ray_t* v);
static const char* jw_type_name(int8_t t);
static bool jw_rows(jw_t* w, ray_t* t, ray_t* vt);

/* Cell i of the vector v. */
static bool jw_cell(jw_t* w, ray_t* v, int64_t i) {
    const uint8_t* d = (const uint8_t*)ray_data(v);
    switch (v->type) {
    case RAY_LIST: return jw_value(w, ((ray_t**)ray_data(v))[i]);
    case RAY_BOOL: return d[i] ? JW_LIT(w, "true") : JW_LIT(w, "false");
    case RAY_U8:   return jw_i64(w, d[i]);
    case RAY_STR: {
        size_t l = 0;
        const char* s = ray_str_vec_get(v, i, &l);
        return jw_string(w, s ? s : "", s ? l : 0);
    }
    case RAY_SYM: {
        ray_t* s = ray_sym_vec_cell(v, i);
        return jw_string(w, s ? ray_str_ptr(s) : "", s ? ray_str_len(s) : 0);
    }
    default: break;
    }
    if (ray_vec_is_null(v, i)) return JW_LIT(w, "null");
    switch (v->type) {
    case RAY_I16: { int16_t x; memcpy(&x, d + i * 2, 2); return jw_i64(w, x); }
    case RAY_I32: { int32_t x; memcpy(&x, d + i * 4, 4); return jw_i64(w, x); }
    case RAY_I64: { int64_t x; memcpy(&x, d + i * 8, 8); return jw_i64(w, x); }
    case RAY_F32: { float x; memcpy(&x, d + i * 4, 4); return jw_f32(w, x); }
    case RAY_F64: { double x; memcpy(&x, d + i * 8, 8); return jw_f64(w, x); }
    case RAY_DATE: { int32_t x; memcpy(&x, d + i * 4, 4); return jw_printed(w, ray_date(x)); }
    case RAY_TIME: { int32_t x; memcpy(&x, d + i * 4, 4); return jw_printed(w, ray_time(x)); }
    case RAY_TIMESTAMP: { int64_t x; memcpy(&x, d + i * 8, 8); return jw_printed(w, ray_timestamp(x)); }
    case RAY_TIMESPAN: { int64_t x; memcpy(&x, d + i * 8, 8); return jw_printed(w, ray_timespan(x)); }
    case RAY_GUID: return jw_printed(w, ray_guid(d + i * 16));
    default:
        w->err = ray_error("type", ".json.format: cannot write a %s value", jw_type_name(v->type));
        return false;
    }
}

/* Key i of a dict.  A symbol or a string is the key; any other scalar
 * (a keyed table's key: a number, a date, a guid, ...) is the text JSON
 * writes for it, as a string; a null scalar the empty string.  A key that
 * is not a scalar (a multi-column key) cannot name a member. */
static bool jw_key(jw_t* w, ray_t* keys, int64_t i) {
    if (keys->type == RAY_STR || keys->type == RAY_SYM) return jw_cell(w, keys, i);
    ray_t* k = keys->type == RAY_LIST ? ((ray_t**)ray_data(keys))[i] : NULL;
    if (k && (k->type == -RAY_STR || k->type == -RAY_SYM)) return jw_value(w, k);
    if (keys->type == RAY_LIST && (!k || !ray_is_atom(k) || RAY_IS_NULL(k))) {
        w->err = ray_error("type", ".json.format: object keys must be scalars, got %s",
                           k ? jw_type_name(k->type) : "null");
        return false;
    }
    jw_t sub;   /* the key's text, untracked (a zeroed tracker) */
    memset(&sub, 0, sizeof sub);
    sub.depth = w->depth;
    bool ok = k ? jw_value(&sub, k) : jw_cell(&sub, keys, i);
    if (!ok) { w->err = sub.err; if (sub.p) ray_free_raw(sub.p); return false; }
    if (sub.len == 4 && memcmp(sub.p, "null", 4) == 0) ok = JW_LIT(w, "\"\"");
    else if (sub.len && sub.p[0] == '"') ok = jw_put(w, sub.p, sub.len);
    else ok = jw_string(w, sub.p, sub.len);
    ray_free_raw(sub.p);
    return ok;
}

/* A type as an error names it: a parted column and the partition key
 * column read as what they are, not as an unnamed tag. */
static const char* jw_type_name(int8_t t) {
    static _Thread_local char b[48];
    if (RAY_IS_PARTED(t)) {
        snprintf(b, sizeof b, "parted %s", ray_type_name((int8_t)RAY_PARTED_BASETYPE(t)));
        return b;
    }
    if (t == RAY_MAPCOMMON) return "partition key";
    if (t == RAY_ENCCOL) return "compressed";
    return ray_type_name(t);
}

/* The rows of `t` (and of `vt`, the value columns of a keyed table whose
 * key columns `t` holds) as an array of objects.  A column that is not a
 * plain vector or list (a parted, a partition key or a compressed one) is
 * read as `at` reads it, one flat vector. */
static bool jw_rows(jw_t* w, ray_t* t, ray_t* vt) {
    ray_t* tabs[2] = { t, vt };
    int ntab = vt ? 2 : 1;
    int64_t nr = ray_table_nrows(t);
    if (vt && ray_table_nrows(vt) != nr) {
        w->err = ray_error("length", ".json.format: a keyed table's keys and values differ in length");
        return false;
    }
    int64_t nc = 0;
    for (int k = 0; k < ntab; k++) nc += ray_table_ncols(tabs[k]);
    ray_t* hdr = NULL;
    ray_t** cols = (ray_t**)scratch_alloc(&hdr, (size_t)(nc > 0 ? nc : 1) * 2 * sizeof(ray_t*));
    if (!cols) { w->err = ray_error("oom", NULL); return false; }
    int64_t* names = (int64_t*)(cols + nc);
    int64_t got = 0;
    bool ok = true;
    for (int k = 0; ok && k < ntab; k++) {
        for (int64_t c = 0; ok && c < ray_table_ncols(tabs[k]); c++) {
            int64_t name = ray_table_col_name(tabs[k], c);
            ray_t* col = ray_table_col_idx(tabs[k], c);
            if (col && RAY_IS_ERR(col)) { w->err = col; ok = false; break; }
            if (col && (col->type == RAY_LIST || ray_is_vec(col))) {
                ray_retain(col);
            } else {
                ray_t* sym = ray_sym(name);
                col = sym && !RAY_IS_ERR(sym) ? ray_at_fn(tabs[k], sym) : sym;
                if (sym && !RAY_IS_ERR(sym)) ray_release(sym);
                if (col && !RAY_IS_ERR(col) && ray_is_lazy(col)) col = ray_lazy_materialize(col);
                if (!col || RAY_IS_ERR(col)) { w->err = col ? col : ray_error("oom", NULL); ok = false; break; }
                if (col->type != RAY_LIST && !ray_is_vec(col)) {
                    w->err = ray_error("type", ".json.format: cannot write a %s column", jw_type_name(col->type));
                    ray_release(col);
                    ok = false;
                    break;
                }
            }
            if (col->len != nr) {
                w->err = ray_error("length", ".json.format: a column of %lld rows in a table of %lld",
                                   (long long)col->len, (long long)nr);
                ray_release(col);
                ok = false;
                break;
            }
            cols[got] = col;
            names[got++] = name;
        }
    }
    ok = ok && JW_LIT(w, "[");
    for (int64_t r = 0; ok && r < nr; r++) {
        if ((r & (RAY_MORSEL_ELEMS - 1)) == 0 && pool_cancelled(NULL)) { w->err = ray_error("cancel", NULL); ok = false; break; }
        ok = (r == 0 || JW_LIT(w, ",")) && JW_LIT(w, "{");
        for (int64_t c = 0; ok && c < got; c++) {
            ray_t* name = ray_sym_str(names[c]);
            ok = (c == 0 || JW_LIT(w, ",")) &&
                 jw_string(w, name ? ray_str_ptr(name) : "", name ? ray_str_len(name) : 0) &&
                 JW_LIT(w, ":") && jw_cell(w, cols[c], r);
        }
        ok = ok && JW_LIT(w, "}");
    }
    ok = ok && JW_LIT(w, "]");
    for (int64_t c = 0; c < got; c++) ray_release(cols[c]);
    scratch_free(hdr);
    return ok;
}

static bool jw_value_c(jw_t* w, ray_t* v);
/* JSON writes a value out in full, a shared part again each time it is
 * met: the bytes written again are counted as they are written, and past
 * RAY_DUP_BUDGET the write stops with limit (#1627, ops/walkmemo.h). */
__attribute__((noinline)) static bool jw_value_watched(jw_t* w, ray_t* v);
static bool jw_value(jw_t* w, ray_t* v) {
    if (RAY_LIKELY(!ray_dup_watch(&w->dup, v))) return jw_value_c(w, v);
    return jw_value_watched(w, v);
}
__attribute__((noinline)) static bool jw_value_watched(jw_t* w, ray_t* v) {
    bool opened = ray_dup_enter(&w->dup, v, w->len);
    if (RAY_UNLIKELY(ray_dup_over(&w->dup, w->len))) {   /* the lower bound refused it */
        ray_dup_leave(&w->dup, opened, w->len);
        w->err = ray_error("limit", ".json.format: value repeats shared parts beyond 1 GiB of extra output");
        return false;
    }
    bool ok = jw_value_c(w, v);
    ray_dup_leave(&w->dup, opened, w->len);
    if (ok && RAY_UNLIKELY(ray_dup_over(&w->dup, w->len))) {
        w->err = ray_error("limit", ".json.format: value repeats shared parts beyond 1 GiB of extra output");
        return false;
    }
    return ok;
}
static bool jw_value_c(jw_t* w, ray_t* v) {
    if (w->err) return false;
    if (!v || RAY_IS_NULL(v)) return JW_LIT(w, "null");
    if (RAY_IS_ERR(v)) { w->err = ray_error("type", ".json.format: cannot write an error value"); return false; }
    if (ray_is_atom(v)) {
        switch (-v->type) {
        case RAY_BOOL: return RAY_ATOM_IS_NULL(v) ? JW_LIT(w, "null") : v->b8 ? JW_LIT(w, "true") : JW_LIT(w, "false");
        case RAY_U8:   return RAY_ATOM_IS_NULL(v) ? JW_LIT(w, "null") : jw_i64(w, v->u8);
        case RAY_STR:  return jw_string(w, ray_str_ptr(v), ray_str_len(v));
        case RAY_SYM: {
            ray_t* s = ray_sym_str(v->i64);
            return jw_string(w, s ? ray_str_ptr(s) : "", s ? ray_str_len(s) : 0);
        }
        default: break;
        }
        if (RAY_ATOM_IS_NULL(v)) return JW_LIT(w, "null");
        switch (-v->type) {
        case RAY_I16: return jw_i64(w, v->i16);
        case RAY_I32: return jw_i64(w, v->i32);
        case RAY_I64: return jw_i64(w, v->i64);
        case RAY_F32: return jw_f32(w, (float)v->f64);
        case RAY_F64: return jw_f64(w, v->f64);
        case RAY_DATE: case RAY_TIME: case RAY_TIMESTAMP: case RAY_TIMESPAN: case RAY_GUID:
            ray_retain(v);
            return jw_printed(w, v);
        default:
            w->err = ray_error("type", ".json.format: cannot write a %s value", jw_type_name(v->type));
            return false;
        }
    }
    if (++w->depth > RAY_READ_DEPTH_CAP) {
        w->err = ray_error("limit", ".json.format: depth limit of %d exceeded", RAY_READ_DEPTH_CAP);
        return false;
    }
    if (ray_stack_short()) {
        w->err = ray_error("limit", ".json.format: nesting too deep for this thread's stack");
        return false;
    }
    bool ok = true;
    if (v->type == RAY_DICT && ray_dict_keys(v) && ray_dict_keys(v)->type == RAY_TABLE &&
        ray_dict_vals(v) && ray_dict_vals(v)->type == RAY_TABLE) {
        ok = jw_rows(w, ray_dict_keys(v), ray_dict_vals(v));   /* a keyed table */
    } else if (v->type == RAY_DICT) {
        ray_t* keys = ray_dict_keys(v);
        ray_t* vals = ray_dict_vals(v);
        if (!keys || !vals || (keys->type != RAY_LIST && !ray_is_vec(keys))) {
            w->err = ray_error("type", ".json.format: object keys must be symbols or strings");
            return false;
        }
        ok = JW_LIT(w, "{");
        for (int64_t i = 0; ok && i < keys->len; i++) {
            if (i && !JW_LIT(w, ",")) { ok = false; break; }
            ok = jw_key(w, keys, i) && JW_LIT(w, ":") && jw_cell(w, vals, i);
        }
        ok = ok && JW_LIT(w, "}");
    } else if (v->type == RAY_TABLE) {
        ok = jw_rows(w, v, NULL);
    } else if (v->type == RAY_LIST || ray_is_vec(v)) {
        ok = JW_LIT(w, "[");
        for (int64_t i = 0; ok && i < v->len; i++) {
            if ((i & (RAY_MORSEL_ELEMS - 1)) == 0 && pool_cancelled(NULL)) { w->err = ray_error("cancel", NULL); ok = false; break; }
            ok = (i == 0 || JW_LIT(w, ",")) && jw_cell(w, v, i);
        }
        ok = ok && JW_LIT(w, "]");
    } else {
        w->err = ray_error("type", ".json.format: cannot write a %s value", jw_type_name(v->type));
        ok = false;
    }
    w->depth--;
    return ok;
}

ray_t* ray_json_format_fn(ray_t* v) {
    ray_t* x = jp_input(v);
    if (x && RAY_IS_ERR(x)) return x;
    jw_t w;
    memset(&w, 0, sizeof w);
    ray_dup_begin(&w.dup);
    ray_dup_root(&w.dup, x, false);
    bool ok = jw_value(&w, x);
    ray_dup_end(&w.dup);
    ray_t* out = ok ? ray_str(w.p ? w.p : "", w.len) : (w.err ? w.err : ray_error("oom", NULL));
    if (ok && w.err) ray_release(w.err);
    if (w.p) ray_free_raw(w.p);
    return out;
}
