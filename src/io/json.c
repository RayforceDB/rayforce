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
#include "lang/readval.h"      /* ray_read_limits_t, ray_read_limits_apply */
#include "lang/format.h"       /* ray_type_name, ray_fmt */
#include "ops/internal.h"      /* pool_cancelled, RAY_IS_PARTED */
#include "ops/hash.h"          /* ray_hash_bytes */
#include "core/numparse.h"     /* ray_parse_f64 */
#include "table/sym.h"
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
        jp->err = ray_error(code, "%s: %s at byte %zu of element %lld", jp->who, what, at, (long long)jp->row);
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

#define JP_ATOM_BYTES 32   /* one atom block, as the charge counts it */

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
    if (jp->end - jp->p < 5) { jp->p = jp->end; jp_fail(jp, "parse", "unexpected end of input in a \\u escape"); return -1; }
    uint32_t v = 0;
    for (int i = 1; i <= 4; i++) {
        int h = hexval(jp->p[i]);
        if (h < 0) { jp->p += i; jp_fail(jp, "parse", "invalid hex digit in a \\u escape"); return -1; }
        v = (v << 4) | (uint32_t)h;
    }
    jp->p += 5;
    return v;
}

static bool jp_buf_need(jp_t* jp, size_t n) {
    if (n <= jp->buf_cap) return true;
    size_t cap = jp->buf_cap ? jp->buf_cap : 64;
    while (cap < n) cap *= 2;
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
            if (!jp_charge(jp, JP_ATOM_BYTES)) return NULL;
            jp->p = q;
            if (!build) return RAY_NULL_OBJ;
            return ray_i64(neg ? -(int64_t)v : (int64_t)v);
        }
    }
    double f = 0.0;
    if (ray_parse_f64((const char*)st, len, &f) != len || !isfinite(f))
        return jp_fail(jp, "domain", "number out of the F64 range");
    if (!jp_charge(jp, JP_ATOM_BYTES)) return NULL;
    jp->p = q;
    if (!build) return RAY_NULL_OBJ;
    return ray_f64(f);
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

static ray_t* jp_array(jp_t* jp, bool build) {
    jp->p++;                                   /* '[' */
    ray_t* list = NULL;
    if (build) {
        list = ray_list_new(4);
        if (!list || RAY_IS_ERR(list)) return jp_fail(jp, "oom", "out of memory");
    }
    jp_ws(jp);
    if (jp->p < jp->end && *jp->p == ']') { jp->p++; return build ? list : RAY_NULL_OBJ; }
    for (;;) {
        ray_t* v = jp_value(jp, build);
        if (!v) { if (list) ray_release(list); return NULL; }
        if (build) {
            if (!jp_bytes(jp, sizeof(ray_t*))) { ray_release(v); ray_release(list); return NULL; }
            list = ray_list_append(list, v);
            ray_release(v);
            if (RAY_IS_ERR(list)) { jp->err = list; return NULL; }
        }
        jp_ws(jp);
        if (jp->p < jp->end && *jp->p == ',') { jp->p++; jp_ws(jp); continue; }
        if (jp->p < jp->end && *jp->p == ']') { jp->p++; break; }
        if (list) ray_release(list);
        return jp_unexpected(jp, "',' or ']'");
    }
    if (!build) return RAY_NULL_OBJ;
    ray_t* out = json_collapse(list, false);
    if (RAY_IS_ERR(out)) { jp->err = out; return NULL; }
    return out;
}

/* Object keys seen so far: their positions in `keys`, by hash of the bytes,
 * open addressing.  Built once an object has more than a few keys. */
typedef struct { int64_t* slot; size_t cap; } jkeys_t;

static int64_t jk_find(ray_t* keys, jkeys_t* h, const char* p, size_t l, uint64_t hv, size_t* slot_out) {
    if (!h->slot) {
        for (int64_t i = 0; i < keys->len; i++) {
            size_t kl = 0;
            const char* kp = ray_str_vec_get(keys, i, &kl);
            if (kl == l && (l == 0 || memcmp(kp, p, l) == 0)) return i;
        }
        return -1;
    }
    size_t s = (size_t)hv & (h->cap - 1);
    for (;;) {
        int64_t i = h->slot[s];
        if (i < 0) { *slot_out = s; return -1; }
        size_t kl = 0;
        const char* kp = ray_str_vec_get(keys, i, &kl);
        if (kl == l && (l == 0 || memcmp(kp, p, l) == 0)) return i;
        s = (s + 1) & (h->cap - 1);
    }
}

static bool jk_grow(jp_t* jp, ray_t* keys, jkeys_t* h) {
    size_t cap = h->cap ? h->cap * 2 : 64;
    if (!jp_charge(jp, cap * sizeof(int64_t))) return false;
    int64_t* ns = (int64_t*)ray_alloc_raw(cap * sizeof(int64_t));
    if (!ns) { jp_fail(jp, "oom", "out of memory"); return false; }
    for (size_t i = 0; i < cap; i++) ns[i] = -1;
    for (int64_t i = 0; i < keys->len; i++) {
        size_t kl = 0;
        const char* kp = ray_str_vec_get(keys, i, &kl);
        size_t s = (size_t)ray_hash_bytes(kp ? kp : "", kl) & (cap - 1);
        while (ns[s] >= 0) s = (s + 1) & (cap - 1);
        ns[s] = i;
    }
    if (h->slot) ray_free_raw(h->slot);
    h->slot = ns;
    h->cap = cap;
    return true;
}

static ray_t* jp_object(jp_t* jp, bool build) {
    jp->p++;                                   /* '{' */
    ray_t* keys = NULL;
    ray_t* vals = NULL;
    jkeys_t h = { NULL, 0 };
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
    for (;;) {
        if (jp->p >= jp->end || *jp->p != '"') { jp_unexpected(jp, "a string key"); goto fail; }
        const char* kp; size_t kl;
        if (!jp_string(jp, &kp, &kl)) goto fail;
        int64_t at = -1;
        size_t slot = 0;
        uint64_t hv = 0;
        if (build) {
            if (!jp_bytes(jp, kl + 8)) goto fail;
            hv = h.slot || keys->len >= 8 ? ray_hash_bytes(kp, kl) : 0;
            if (!h.slot && keys->len >= 8 && !jk_grow(jp, keys, &h)) goto fail;
            at = jk_find(keys, &h, kp, kl, hv, &slot);
            if (at < 0) {
                keys = ray_str_vec_append(keys, kp, kl);
                if (RAY_IS_ERR(keys)) { jp->err = keys; keys = NULL; goto fail; }
                if (h.slot) {
                    h.slot[slot] = keys->len - 1;
                    if ((size_t)keys->len * 2 > h.cap && !jk_grow(jp, keys, &h)) goto fail;
                }
            }
        }
        jp_ws(jp);
        if (jp->p >= jp->end || *jp->p != ':') { jp_unexpected(jp, "':'"); goto fail; }
        jp->p++;
        jp_ws(jp);
        ray_t* v = jp_value(jp, build);
        if (!v) goto fail;
        if (build) {
            if (at < 0) {
                vals = ray_list_append(vals, v);
                ray_release(v);
                if (RAY_IS_ERR(vals)) { jp->err = vals; vals = NULL; goto fail; }
            } else {
                /* a repeated key: its first position, its last value */
                ray_t** items = (ray_t**)ray_data(vals);
                ray_release(items[at]);
                items[at] = v;
            }
        }
        jp_ws(jp);
        if (jp->p < jp->end && *jp->p == ',') { jp->p++; jp_ws(jp); continue; }
        if (jp->p < jp->end && *jp->p == '}') { jp->p++; break; }
        jp_unexpected(jp, "',' or '}'");
        goto fail;
    }
done:
    if (h.slot) ray_free_raw(h.slot);
    if (!build) return RAY_NULL_OBJ;
    out = ray_dict_new(keys, vals);
    if (RAY_IS_ERR(out)) { jp->err = out; return NULL; }
    return out;
fail:
    if (h.slot) ray_free_raw(h.slot);
    if (keys) ray_release(keys);
    if (vals) ray_release(vals);
    return NULL;
}

/* One value at jp->p (whitespace before it skipped by the caller).  Owned
 * value, RAY_NULL_OBJ when `build` is false, NULL with jp->err set on a
 * failure. */
static ray_t* jp_value(jp_t* jp, bool build) {
    if (jp->p >= jp->end) return jp_unexpected(jp, "a value");
    switch (*jp->p) {
    case '{': case '[': {
        if (++jp->depth > jp->max_depth) return jp_fail(jp, "limit", "depth limit of %zu exceeded", jp->max_depth);
        if (!jp_charge(jp, JP_ATOM_BYTES)) return NULL;
        ray_t* v = *jp->p == '{' ? jp_object(jp, build) : jp_array(jp, build);
        jp->depth--;
        return v;
    }
    case '"': {
        const char* sp; size_t sl;
        if (!jp_string(jp, &sp, &sl)) return NULL;
        if (!jp_charge(jp, JP_ATOM_BYTES + sl)) return NULL;
        if (!build) return RAY_NULL_OBJ;
        ray_t* s = ray_str(sp, sl);
        if (!s || RAY_IS_ERR(s)) { jp->err = s ? s : ray_error("oom", NULL); return NULL; }
        return s;
    }
    case 't':
        if (!jp_literal(jp, "true", 4) || !jp_charge(jp, JP_ATOM_BYTES)) return NULL;
        return build ? ray_bool(true) : RAY_NULL_OBJ;
    case 'f':
        if (!jp_literal(jp, "false", 5) || !jp_charge(jp, JP_ATOM_BYTES)) return NULL;
        return build ? ray_bool(false) : RAY_NULL_OBJ;
    case 'n':
        if (!jp_literal(jp, "null", 4) || !jp_charge(jp, 0)) return NULL;
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
    jp->depth = jp->nodes = jp->bytes = 0;
    if (len > lim->max_input) {
        jp_fail(jp, "limit", "input limit of %zu bytes exceeded (%zu bytes)", lim->max_input, len);
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

/* Split the path at its dots.  A segment of digits (no leading zero) is also
 * an array index. */
static ray_t* json_path(ray_t* path, jseg_t* segs, int* nseg) {
    const char* p;
    size_t l;
    if (path && path->type == -RAY_STR) { p = ray_str_ptr(path); l = ray_str_len(path); }
    else if (path && path->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(path->i64);
        p = s ? ray_str_ptr(s) : "";
        l = s ? ray_str_len(s) : 0;
    } else return ray_error("type", ".json.get: path must be a string, got %s", path ? ray_type_name(path->type) : "null");
    if (l == 0) return ray_error("domain", ".json.get: path is empty");
    int k = 0;
    size_t st = 0;
    for (size_t i = 0; i <= l; i++) {
        if (i < l && p[i] != '.') continue;
        if (i == st) return ray_error("domain", ".json.get: empty segment in path at byte %zu", i);
        if (k == JSON_PATH_MAX) return ray_error("limit", ".json.get: path has more than %d segments", JSON_PATH_MAX);
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
    if (!jp_charge(jp, 0)) return NULL;
    jp->p++;
    ray_t* found = RAY_NULL_OBJ;
    jp_ws(jp);
    if (c == '{') {
        if (jp->p < jp->end && *jp->p == '}') { jp->p++; goto done; }
        for (;;) {
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
} jp_get_ctx_t;

static ray_t* jp_get_doc(jp_t* jp, const char* s, size_t l, int64_t row, jp_get_ctx_t* c) {
    if (l == 0) return RAY_NULL_OBJ;           /* a null document: no field */
    if (!jp_doc(jp, s, l, row, c->lim)) return NULL;
    jp_ws(jp);
    ray_t* v = jp_nav(jp, c->segs, c->nseg, 0);
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

ray_t* ray_json_get_fn(ray_t* s, ray_t* path) {
    jseg_t segs[JSON_PATH_MAX];
    int nseg = 0;
    ray_t* perr = json_path(path, segs, &nseg);
    if (perr) return perr;
    ray_t* x = jp_input(s);
    if (!x || RAY_IS_ERR(x)) return x;
    ray_read_limits_t lim = ray_read_limits_default();
    jp_t jp;
    jp_init(&jp, ".json.get", &lim);
    jp_get_ctx_t c = { NULL, &lim, segs, nseg };
    if (x->type == -RAY_STR)
        return jp_finish(&jp, jp_get_doc(&jp, ray_str_ptr(x), ray_str_len(x), -1, &c));
    if (!jp_is_docs(x))
        return ray_error("type", ".json.get: expected a string or a string vector, got %s", ray_type_name(x->type));
    int64_t n = RAY_IS_PARTED(x->type) ? ray_parted_nrows(x) : x->len;
    c.out = ray_list_new(n > 0 ? n : 1);
    if (!c.out || RAY_IS_ERR(c.out)) return c.out ? c.out : ray_error("oom", NULL);
    ray_t* err = jp_docs(&jp, x, jp_get_each, &c);
    if (jp.buf) ray_free_raw(jp.buf);
    if (err) {
        if (c.out) ray_release(c.out);
        return err;
    }
    if (c.out->len == 0) {                     /* no rows: an empty STR */
        ray_release(c.out);
        return ray_vec_new(RAY_STR, 0);
    }
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

/* The shortest decimal that reads back as `d`, with a '.' or an exponent so
 * that it reads back as an F64. */
static bool jw_f64(jw_t* w, double d) {
    if (d != d) return JW_LIT(w, "null");
    if (!isfinite(d)) { w->err = ray_error("domain", ".json.format: a float is not finite"); return false; }
    char b[40];
    int k = 0;
    for (int prec = 15; prec <= 17; prec++) {
        k = snprintf(b, sizeof b, "%.*g", prec, d);
        double back = 0.0;
        if (ray_parse_f64(b, (size_t)k, &back) == (size_t)k && back == d) break;
    }
    bool marked = false;
    for (int i = 0; i < k; i++) if (b[i] == '.' || b[i] == 'e' || b[i] == 'E') marked = true;
    if (!marked && k + 2 < (int)sizeof b) { b[k++] = '.'; b[k++] = '0'; }
    return jw_put(w, b, (size_t)k);
}

/* An F32 as the shortest decimal that reads back as that F32. */
static bool jw_f32(jw_t* w, float f) {
    if (f != f) return JW_LIT(w, "null");
    if (!isfinite(f)) { w->err = ray_error("domain", ".json.format: a float is not finite"); return false; }
    char b[40];
    int k = 0;
    for (int prec = 6; prec <= 9; prec++) {
        k = snprintf(b, sizeof b, "%.*g", prec, (double)f);
        double back = 0.0;
        if (ray_parse_f64(b, (size_t)k, &back) == (size_t)k && (float)back == f) break;
    }
    bool marked = false;
    for (int i = 0; i < k; i++) if (b[i] == '.' || b[i] == 'e' || b[i] == 'E') marked = true;
    if (!marked && k + 2 < (int)sizeof b) { b[k++] = '.'; b[k++] = '0'; }
    return jw_put(w, b, (size_t)k);
}

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
        w->err = ray_error("type", ".json.format: cannot write a %s value", ray_type_name(v->type));
        return false;
    }
}

static bool jw_key(jw_t* w, ray_t* keys, int64_t i) {
    if (keys->type == RAY_STR || keys->type == RAY_SYM) return jw_cell(w, keys, i);
    if (keys->type == RAY_LIST) {
        ray_t* k = ((ray_t**)ray_data(keys))[i];
        if (k && (k->type == -RAY_STR || k->type == -RAY_SYM)) return jw_value(w, k);
    }
    w->err = ray_error("type", ".json.format: object keys must be symbols or strings, got %s",
                       ray_type_name(keys->type == RAY_LIST ? ((ray_t**)ray_data(keys))[i]->type : keys->type));
    return false;
}

static bool jw_value(jw_t* w, ray_t* v) {
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
            w->err = ray_error("type", ".json.format: cannot write a %s value", ray_type_name(v->type));
            return false;
        }
    }
    if (++w->depth > RAY_READ_DEPTH_CAP) {
        w->err = ray_error("limit", ".json.format: depth limit of %d exceeded", RAY_READ_DEPTH_CAP);
        return false;
    }
    bool ok = true;
    if (v->type == RAY_DICT) {
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
        int64_t nc = ray_table_ncols(v), nr = ray_table_nrows(v);
        ray_t* hdr = NULL;
        ray_t** cols = (ray_t**)scratch_alloc(&hdr, (size_t)(nc > 0 ? nc : 1) * sizeof(ray_t*));
        if (!cols) { w->err = ray_error("oom", NULL); return false; }
        for (int64_t c = 0; c < nc; c++) {
            ray_t* col = ray_table_col_idx(v, c);
            if (col && RAY_IS_ERR(col)) { w->err = col; scratch_free(hdr); return false; }
            if (!col || (col->type != RAY_LIST && !ray_is_vec(col))) {
                w->err = ray_error("type", ".json.format: cannot write a %s column", col ? ray_type_name(col->type) : "missing");
                scratch_free(hdr);
                return false;
            }
            cols[c] = col;
        }
        ok = JW_LIT(w, "[");
        for (int64_t r = 0; ok && r < nr; r++) {
            if ((r & (RAY_MORSEL_ELEMS - 1)) == 0 && pool_cancelled(NULL)) { w->err = ray_error("cancel", NULL); ok = false; break; }
            ok = (r == 0 || JW_LIT(w, ",")) && JW_LIT(w, "{");
            for (int64_t c = 0; ok && c < nc; c++) {
                ray_t* name = ray_sym_str(ray_table_col_name(v, c));
                ok = (c == 0 || JW_LIT(w, ",")) &&
                     jw_string(w, name ? ray_str_ptr(name) : "", name ? ray_str_len(name) : 0) &&
                     JW_LIT(w, ":") && jw_cell(w, cols[c], r);
            }
            ok = ok && JW_LIT(w, "}");
        }
        ok = ok && JW_LIT(w, "]");
        scratch_free(hdr);
    } else if (v->type == RAY_LIST || ray_is_vec(v)) {
        ok = JW_LIT(w, "[");
        for (int64_t i = 0; ok && i < v->len; i++) {
            if ((i & (RAY_MORSEL_ELEMS - 1)) == 0 && pool_cancelled(NULL)) { w->err = ray_error("cancel", NULL); ok = false; break; }
            ok = (i == 0 || JW_LIT(w, ",")) && jw_cell(w, v, i);
        }
        ok = ok && JW_LIT(w, "]");
    } else {
        w->err = ray_error("type", ".json.format: cannot write a %s value", ray_type_name(v->type));
        ok = false;
    }
    w->depth--;
    return ok;
}

ray_t* ray_json_format_fn(ray_t* v) {
    ray_t* x = jp_input(v);
    if (x && RAY_IS_ERR(x)) return x;
    jw_t w = { NULL, 0, 0, NULL, 0 };
    bool ok = jw_value(&w, x);
    ray_t* out = ok ? ray_str(w.p ? w.p : "", w.len) : (w.err ? w.err : ray_error("oom", NULL));
    if (ok && w.err) ray_release(w.err);
    if (w.p) ray_free_raw(w.p);
    return out;
}
