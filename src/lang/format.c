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

#include "lang/format.h"
#include "lang/env.h"
#include "table/sym.h"
#include "lang/eval.h"
#include "ops/ops.h"    /* RAY_LAZY, ray_lazy_materialize */
#include "mem/heap.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <inttypes.h>
#include <limits.h>

/* ===== Internal growable buffer ===== */

typedef struct {
    char*   buf;
    int32_t len;
    int32_t cap;
    ray_t*  block;  /* ray_alloc'd backing block */
} fmt_buf_t;

static void fmt_init(fmt_buf_t* b) {
    b->block = ray_alloc(256);
    b->buf   = (char*)ray_data(b->block);
    b->len   = 0;
    b->cap   = 256;
}

static void fmt_destroy(fmt_buf_t* b) {
    if (b->block) {
        ray_free(b->block);
        b->block = NULL;
        b->buf   = NULL;
        b->len   = 0;
        b->cap   = 0;
    }
}

static void fmt_ensure(fmt_buf_t* b, int32_t extra) {
    if (b->len + extra <= b->cap) return;
    int32_t new_cap = b->cap;
    while (new_cap < b->len + extra)
        new_cap *= 2;
    ray_t* new_block = ray_alloc((size_t)new_cap);
    char*  new_buf   = (char*)ray_data(new_block);
    memcpy(new_buf, b->buf, (size_t)b->len);
    ray_free(b->block);
    b->block = new_block;
    b->buf   = new_buf;
    b->cap   = new_cap;
}

static void fmt_putc(fmt_buf_t* b, char c) {
    fmt_ensure(b, 1);
    b->buf[b->len++] = c;
}

static void fmt_puts(fmt_buf_t* b, const char* s) {
    int32_t slen = (int32_t)strlen(s);
    fmt_ensure(b, slen);
    memcpy(b->buf + b->len, s, (size_t)slen);
    b->len += slen;
}

static void fmt_printf(fmt_buf_t* b, const char* fmt, ...) {
    va_list ap;

    /* Try to fit in remaining space first */
    va_start(ap, fmt);
    int32_t avail = b->cap - b->len;
    int n = vsnprintf(b->buf + b->len, (size_t)avail, fmt, ap);
    va_end(ap);

    if (n < 0) return; /* encoding error */

    if (n < avail) {
        b->len += n;
        return;
    }

    /* Need more space — grow and retry */
    fmt_ensure(b, n + 1);
    va_start(ap, fmt);
    vsnprintf(b->buf + b->len, (size_t)(b->cap - b->len), fmt, ap);
    va_end(ap);
    b->len += n;
}

static void fmt_putn(fmt_buf_t* b, const char* s, int32_t n) {
    fmt_ensure(b, n);
    memcpy(b->buf + b->len, s, (size_t)n);
    b->len += n;
}

static ray_t* fmt_to_str(fmt_buf_t* b) {
    ray_t* result = ray_str(b->buf, (size_t)b->len);
    fmt_destroy(b);
    return result;
}

/* ===== Static globals ===== */

static int g_precision = FMT_DEFAULT_PRECISION;
static int g_row_width = FMT_DEFAULT_ROW_WIDTH;

/* ===== Public API ===== */

void ray_fmt_set_precision(int digits) {
    if (digits >= 0 && digits <= 20)
        g_precision = digits;
}

void ray_fmt_set_width(int cols) {
    if (cols > 0)
        g_row_width = cols;
}

/* Single type-name function. Negative type (atom) → lowercase,
 * positive type (vector/collection) → uppercase. */
const char* ray_type_name(int8_t type) {
    switch (type < 0 ? -type : type) {
    case RAY_BOOL:      return type < 0 ? "b8"        : "B8";
    case RAY_U8:        return type < 0 ? "u8"        : "U8";
    case RAY_I16:       return type < 0 ? "i16"       : "I16";
    case RAY_I32:       return type < 0 ? "i32"       : "I32";
    case RAY_I64:       return type < 0 ? "i64"       : "I64";
    case RAY_F32:       return type < 0 ? "f32"       : "F32";
    case RAY_F64:       return type < 0 ? "f64"       : "F64";
    case RAY_DATE:      return type < 0 ? "date"      : "DATE";
    case RAY_TIME:      return type < 0 ? "time"      : "TIME";
    case RAY_TIMESTAMP: return type < 0 ? "timestamp" : "TIMESTAMP";
    case RAY_SYM:       return type < 0 ? "sym"       : "SYM";
    case RAY_STR:       return type < 0 ? "str"       : "STR";
    case RAY_GUID:      return type < 0 ? "guid"      : "GUID";
    case RAY_TABLE:     return "TABLE";
    case RAY_DICT:      return "DICT";
    case RAY_LIST:      return "LIST";
    case RAY_INDEX:     return "INDEX";
    default:            return "?";
    }
}

/* ===== Atom formatters ===== */

static void fmt_bool(fmt_buf_t* b, uint8_t val) {
    fmt_puts(b, val ? "true" : "false");
}

static void fmt_u8(fmt_buf_t* b, uint8_t val) {
    fmt_printf(b, "0x%02x", val);
}


static void fmt_i16(fmt_buf_t* b, int16_t val) {
    fmt_printf(b, "%d", (int)val);
}

static void fmt_i32(fmt_buf_t* b, int32_t val) {
    fmt_printf(b, "%d", (int)val);
}

static void fmt_i64(fmt_buf_t* b, int64_t val) {
    fmt_printf(b, "%" PRId64, val);
}

static void fmt_f64(fmt_buf_t* b, double val) {
    if (val == -0.0 && signbit(val)) val = 0.0; /* normalize -0.0 */
    if (val == 0.0) {
        /* Zero: format as "0.0" (after trailing-zero strip) */
        char tmp[16];
        int n = snprintf(tmp, sizeof(tmp), "%.*f", g_precision, 0.0);
        char* dot = strchr(tmp, '.');
        if (dot) { char* end = tmp + n - 1; while (end > dot + 1 && *end == '0') end--; n = (int)(end - tmp + 1); }
        fmt_putn(b, tmp, (int32_t)n);
        return;
    }
    double absval = val < 0 ? -val : val;
    double order = log10(absval);

    /* Format with requested precision */
    char tmp[64];
    int n;
    if (val != 0.0 && (order > 6 || order < -1))
        n = snprintf(tmp, sizeof(tmp), "%.*e", g_precision, val);
    else
        n = snprintf(tmp, sizeof(tmp), "%.*f", g_precision, val);

    if (n <= 0 || n >= (int)sizeof(tmp)) {
        fmt_puts(b, "?");
        return;
    }

    /* Strip trailing zeros after decimal point, keeping at least one
     * digit after '.'.  Do NOT touch exponential notation. */
    char* dot = strchr(tmp, '.');
    char* e   = strchr(tmp, 'e');
    if (dot && !e) {
        char* end = tmp + n - 1;
        while (end > dot + 1 && *end == '0')
            end--;
        n = (int)(end - tmp + 1);
    }

    fmt_putn(b, tmp, (int32_t)n);
}

static void fmt_f32(fmt_buf_t* b, float val) {
    fmt_f64(b, (double)val);
}

static void fmt_guid(fmt_buf_t* b, const uint8_t* bytes) {
    static const char hex[] = "0123456789abcdef";
    /* Format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx */
    static const int groups[] = {4, 2, 2, 2, 6};
    int pos = 0;
    for (int g = 0; g < 5; g++) {
        if (g > 0) fmt_putc(b, '-');
        for (int j = 0; j < groups[g]; j++) {
            fmt_putc(b, hex[bytes[pos] >> 4]);
            fmt_putc(b, hex[bytes[pos] & 0x0F]);
            pos++;
        }
    }
}

static void fmt_sym(fmt_buf_t* b, int64_t sym_id) {
    ray_t* s = ray_sym_str(sym_id);
    if (s && !RAY_IS_ERR(s)) {
        const char* p = ray_str_ptr(s);
        size_t      n = ray_str_len(s);
        fmt_putn(b, p, (int32_t)n);
        ray_release(s);
    } else {
        fmt_puts(b, "0Ns");
    }
}

/* ===== Date/time/timestamp helpers ===== */

#include "lang/cal.h"

static void time_to_hms(int32_t ms, int* h, int* min, int* s, int* ms_out) {
    int32_t mask = ms >> 31;
    int32_t val  = (mask ^ ms) - mask;  /* absolute value */

    int32_t secs = val / 1000;
    *ms_out = (int)(val % 1000);
    *h      = (int)(secs / 3600);
    int32_t rem = secs % 3600;
    *min    = (int)(rem / 60);
    *s      = (int)(rem % 60);
}

#define NSECS_IN_DAY ((int64_t)24 * 60 * 60 * 1000000000LL)

static void ts_to_parts(int64_t ns, int* y, int* mo, int* d,
                         int* h, int* mi, int* s, int* nanos) {
    int64_t days = ns / NSECS_IN_DAY;
    int64_t span = ns % NSECS_IN_DAY;

    if (span < 0) {
        days -= 1;
        span += NSECS_IN_DAY;
    }

    date_to_ymd((int32_t)days, y, mo, d);

    /* timespan_from_nanos */
    int64_t secs = span / 1000000000LL;
    *nanos = (int)(span % 1000000000LL);
    *h  = (int)(secs / 3600);
    int64_t rem = secs % 3600;
    *mi = (int)(rem / 60);
    *s  = (int)(rem % 60);
}

static void fmt_date(fmt_buf_t* b, int32_t val) {
    int y, m, d;
    date_to_ymd(val, &y, &m, &d);
    fmt_printf(b, "%04d.%02d.%02d", y, m, d);
}

static void fmt_time(fmt_buf_t* b, int32_t val) {
    int h, m, s, ms;
    time_to_hms(val, &h, &m, &s, &ms);
    if (val < 0) fmt_putc(b, '-');
    fmt_printf(b, "%02d:%02d:%02d.%03d", h, m, s, ms);
}

static void fmt_timestamp(fmt_buf_t* b, int64_t val) {
    int y, mo, d, h, mi, s, ns;
    ts_to_parts(val, &y, &mo, &d, &h, &mi, &s, &ns);
    fmt_printf(b, "%04d.%02d.%02dD%02d:%02d:%02d.%09d", y, mo, d, h, mi, s, ns);
}

static void fmt_str_atom(fmt_buf_t* b, ray_t* obj, int full) {
    (void)full;
    const char* p = ray_str_ptr(obj);
    size_t      n = ray_str_len(obj);
    fmt_putc(b, '"');
    fmt_putn(b, p, (int32_t)n);
    fmt_putc(b, '"');
}

/* ===== Forward declarations ===== */

static void fmt_obj(fmt_buf_t* b, ray_t* obj, int mode);

/* ===== Null literal display (type → "0Nx" string) ===== */

static const char* null_literal(int8_t type) {
    switch (type) {
    case RAY_BOOL:      return "0Nb";
    case RAY_U8:        return "0Nu";
    case RAY_I16:       return "0Nh";
    case RAY_I32:       return "0Ni";
    case RAY_I64:       return "0Nl";
    case RAY_F64:       return "0Nf";
    case RAY_F32:       return "0Ne";
    case RAY_DATE:      return "0Nd";
    case RAY_TIME:      return "0Nt";
    case RAY_TIMESTAMP: return "0Np";
    case RAY_SYM:       return "0Ns";
    case RAY_STR:       return "0Nc";
    case RAY_GUID:      return "0Ng";
    default:            return "null";
    }
}

/* ===== Vector element formatter ===== */

static void fmt_raw_elem(fmt_buf_t* b, ray_t* vec, int64_t idx) {
    /* Check for null */
    if (ray_vec_is_null(vec, idx)) {
        fmt_puts(b, null_literal(vec->type));
        return;
    }

    switch (vec->type) {
    case RAY_BOOL:      fmt_bool(b, ((bool*)ray_data(vec))[idx]); break;
    case RAY_U8:        fmt_u8(b, ((uint8_t*)ray_data(vec))[idx]); break;

    case RAY_I16:       fmt_i16(b, ((int16_t*)ray_data(vec))[idx]); break;
    case RAY_I32:       fmt_i32(b, ((int32_t*)ray_data(vec))[idx]); break;
    case RAY_I64:       fmt_i64(b, ((int64_t*)ray_data(vec))[idx]); break;
    case RAY_F32:       fmt_f32(b, ((float*)ray_data(vec))[idx]); break;
    case RAY_F64:       fmt_f64(b, ((double*)ray_data(vec))[idx]); break;
    case RAY_DATE:      fmt_date(b, ((int32_t*)ray_data(vec))[idx]); break;
    case RAY_TIME:      fmt_time(b, ((int32_t*)ray_data(vec))[idx]); break;
    case RAY_TIMESTAMP: fmt_timestamp(b, ((int64_t*)ray_data(vec))[idx]); break;
    case RAY_SYM: {
        int64_t sym_id = ray_read_sym(ray_data(vec), idx, vec->type, vec->attrs);
        fmt_sym(b, sym_id);
        break;
    }
    case RAY_STR: {
        size_t slen = 0;
        const char* p = ray_str_vec_get(vec, idx, &slen);
        if (p) {
            fmt_putc(b, '"');
            fmt_putn(b, p, (int32_t)slen);
            fmt_putc(b, '"');
        }
        break;
    }
    case RAY_GUID:
        fmt_guid(b, ((uint8_t*)ray_data(vec)) + idx * 16);
        break;
    case RAY_LIST: {
        ray_t* child = ((ray_t**)ray_data(vec))[idx];
        if (child) {
            ray_t* s = ray_fmt(child, 1);
            if (s && !RAY_IS_ERR(s)) {
                fmt_putn(b, ray_str_ptr(s), (int32_t)ray_str_len(s));
                ray_release(s);
            } else {
                fmt_puts(b, "?");
            }
        } else {
            fmt_puts(b, "null");
        }
        break;
    }
    default:
        fmt_puts(b, "?");
        break;
    }
}

/* ===== Vector formatter ===== */

static void fmt_vector(fmt_buf_t* b, ray_t* vec, int limit) {
    int64_t len = ray_len(vec);
    if (len == 0) { fmt_puts(b, "[]"); return; }

    fmt_puts(b, "[");
    int32_t start_len = b->len;

    for (int64_t i = 0; i < len; i++) {
        if (i > 0) fmt_putc(b, ' ');

        int32_t before = b->len;
        fmt_raw_elem(b, vec, i);

        /* Width limiting: check if we exceeded the limit */
        if (limit > 0 && (b->len - start_len) > limit) {
            /* Rewind to before this element and truncate */
            b->len = before;
            fmt_puts(b, "..]");
            return;
        }
    }

    fmt_puts(b, "]");
}

/* ===== List formatter ===== */

static void fmt_list(fmt_buf_t* b, ray_t* list, int mode) {
    int64_t len = ray_len(list);
    if (len == 0) { fmt_puts(b, "()"); return; }

    /* Homogeneous atom list → format as vector [...] */
    ray_t** items = (ray_t**)ray_data(list);
    if (items && len > 0 && items[0] && !RAY_IS_ERR(items[0]) && ray_is_atom(items[0])) {
        int8_t first_type = items[0]->type;
        int homogeneous = 1;
        for (int64_t i = 1; i < len; i++) {
            if (!items[i] || RAY_IS_ERR(items[i]) || items[i]->type != first_type) {
                homogeneous = 0; break;
            }
        }
        if (homogeneous) {
            fmt_puts(b, "[");
            for (int64_t i = 0; i < len; i++) {
                if (i > 0) fmt_putc(b, ' ');
                fmt_obj(b, items[i], mode);
            }
            fmt_puts(b, "]");
            return;
        }
    }

    /* mode 0 = compact/round-trippable: "(list ...)" prefix required
     * mode 1 = REPL display: "(...)" matching rayforce 1 output */
    if (mode == 0)
        fmt_puts(b, "(list ");
    else
        fmt_puts(b, "(");

    int64_t max_elems = (mode == 1) ? FMT_LIST_MAX_HEIGHT : len;
    int64_t show = len < max_elems ? len : max_elems;

    for (int64_t i = 0; i < show; i++) {
        if (i > 0) fmt_putc(b, ' ');
        ray_t* elem = ray_list_get(list, i);
        fmt_obj(b, elem, mode);
    }

    if (len > show) fmt_puts(b, " ..");
    fmt_puts(b, ")");
}

/* ===== Dict formatter ===== */

static void fmt_dict(fmt_buf_t* b, ray_t* dict, int mode) {
    ray_t* keys = ray_dict_keys(dict);
    ray_t* vals = ray_dict_vals(dict);
    int64_t npairs = keys ? keys->len : 0;
    if (npairs == 0) { fmt_puts(b, "{}"); return; }

    int64_t max_pairs = (mode == 1) ? FMT_LIST_MAX_HEIGHT : npairs;
    int64_t show = npairs < max_pairs ? npairs : max_pairs;

    fmt_puts(b, "{");
    for (int64_t i = 0; i < show; i++) {
        if (i > 0) fmt_putc(b, ' ');
        /* Render key: synthesize an atom view from the keys vector.  When
         * the source slot is flagged null in the keys' bitmap, set the
         * synthesized atom's nullmap bit 0 so fmt_obj renders the proper
         * null literal.  Without this, nullable GUID/STR/sym keys render
         * as their underlying bytes (e.g. the 16-zero-byte GUID), losing
         * null semantics. */
        bool k_is_null = (keys->type != RAY_LIST) && ray_vec_is_null(keys, i);
        ray_t k_atom_storage;
        ray_t* k_atom = NULL;
        memset(&k_atom_storage, 0, sizeof(k_atom_storage));
        bool k_owned = false;   /* true if k_atom is a fresh allocation */
        if (keys->type == RAY_SYM) {
            k_atom_storage.type = -RAY_SYM;
            k_atom_storage.i64  = ray_read_sym(ray_data(keys), i, RAY_SYM, keys->attrs);
            k_atom = &k_atom_storage;
        } else if (keys->type == RAY_STR) {
            size_t slen = 0;
            const char* sp = ray_str_vec_get(keys, i, &slen);
            k_atom = ray_str(sp ? sp : "", sp ? slen : 0);
            k_owned = true;
        } else if (keys->type == RAY_I64 || keys->type == RAY_TIMESTAMP) {
            k_atom_storage.type = (int8_t)-keys->type;
            k_atom_storage.i64  = ((int64_t*)ray_data(keys))[i];
            k_atom = &k_atom_storage;
        } else if (keys->type == RAY_I32 || keys->type == RAY_DATE || keys->type == RAY_TIME) {
            k_atom_storage.type = (int8_t)-keys->type;
            k_atom_storage.i32  = ((int32_t*)ray_data(keys))[i];
            k_atom = &k_atom_storage;
        } else if (keys->type == RAY_I16) {
            k_atom_storage.type = -RAY_I16;
            k_atom_storage.i16  = ((int16_t*)ray_data(keys))[i];
            k_atom = &k_atom_storage;
        } else if (keys->type == RAY_BOOL || keys->type == RAY_U8) {
            k_atom_storage.type = (int8_t)-keys->type;
            k_atom_storage.u8   = ((uint8_t*)ray_data(keys))[i];
            k_atom = &k_atom_storage;
        } else if (keys->type == RAY_F64) {
            k_atom_storage.type = -RAY_F64;
            k_atom_storage.f64  = ((double*)ray_data(keys))[i];
            k_atom = &k_atom_storage;
        } else if (keys->type == RAY_F32) {
            k_atom_storage.type = -RAY_F32;
            k_atom_storage.f64  = (double)((float*)ray_data(keys))[i];
            k_atom = &k_atom_storage;
        } else if (keys->type == RAY_GUID) {
            /* GUID atoms keep their 16-byte payload in a heap-allocated
             * child block; the stack-local view trick from the other
             * branches doesn't carry the bytes (fmt_obj would deref a
             * bogus inline data[] pointer).  Build a real atom. */
            k_atom = ray_guid(((const uint8_t*)ray_data(keys)) + i * 16);
            k_owned = (k_atom && !RAY_IS_ERR(k_atom));
        } else if (keys->type == RAY_LIST) {
            /* Borrowed — do NOT release. */
            k_atom = ((ray_t**)ray_data(keys))[i];
        }
        if (k_is_null && k_atom) k_atom->nullmap[0] |= 1;
        if (k_atom) fmt_obj(b, k_atom, mode);
        fmt_putc(b, ':');

        /* Render value: borrow from vals (LIST) or synthesize a typed atom
         * directly from index i (do NOT route through k_atom — for STR keys
         * k_atom is a fresh allocation we'll release just below).  */
        if (vals && vals->type == RAY_LIST) {
            ray_t* v = ray_list_get(vals, i);
            fmt_obj(b, v, mode);
        } else if (vals && i < vals->len) {
            bool v_is_null = ray_vec_is_null(vals, i);
            ray_t v_storage; memset(&v_storage, 0, sizeof(v_storage));
            ray_t* v_atom = NULL;
            bool   v_owned = false;
            switch (vals->type) {
                case RAY_BOOL:
                case RAY_U8:        v_storage.type = (int8_t)-vals->type;
                                    v_storage.u8   = ((uint8_t*)ray_data(vals))[i];
                                    v_atom = &v_storage; break;
                case RAY_I16:       v_storage.type = -RAY_I16;
                                    v_storage.i16  = ((int16_t*)ray_data(vals))[i];
                                    v_atom = &v_storage; break;
                case RAY_I32:
                case RAY_DATE:
                case RAY_TIME:      v_storage.type = (int8_t)-vals->type;
                                    v_storage.i32  = ((int32_t*)ray_data(vals))[i];
                                    v_atom = &v_storage; break;
                case RAY_I64:
                case RAY_TIMESTAMP: v_storage.type = (int8_t)-vals->type;
                                    v_storage.i64  = ((int64_t*)ray_data(vals))[i];
                                    v_atom = &v_storage; break;
                case RAY_F32:       v_storage.type = -RAY_F32;
                                    v_storage.f64  = (double)((float*)ray_data(vals))[i];
                                    v_atom = &v_storage; break;
                case RAY_F64:       v_storage.type = -RAY_F64;
                                    v_storage.f64  = ((double*)ray_data(vals))[i];
                                    v_atom = &v_storage; break;
                case RAY_SYM:       v_storage.type = -RAY_SYM;
                                    v_storage.i64  = ray_read_sym(ray_data(vals), i, RAY_SYM, vals->attrs);
                                    v_atom = &v_storage; break;
                case RAY_STR: {
                    size_t vl = 0;
                    const char* vp = ray_str_vec_get(vals, i, &vl);
                    v_atom = ray_str(vp ? vp : "", vp ? vl : 0);
                    v_owned = true;
                    break;
                }
                case RAY_GUID:
                    v_atom = ray_guid(((const uint8_t*)ray_data(vals)) + i * 16);
                    v_owned = (v_atom && !RAY_IS_ERR(v_atom));
                    break;
                default: break;
            }
            if (v_is_null && v_atom) v_atom->nullmap[0] |= 1;
            if (v_atom) fmt_obj(b, v_atom, mode);
            if (v_owned && v_atom) ray_release(v_atom);
        }

        if (k_owned && k_atom) ray_release(k_atom);
    }
    if (npairs > show) fmt_puts(b, " ..");
    fmt_puts(b, "}");
}

/* ===== Box-drawing glyphs (UTF-8) ===== */

#define G_TL "\xe2\x94\x8c"    /* ┌ */
#define G_TR "\xe2\x94\x90"    /* ┐ */
#define G_BL "\xe2\x94\x94"    /* └ */
#define G_BR "\xe2\x94\x98"    /* ┘ */
#define G_H  "\xe2\x94\x80"    /* ─ */
#define G_V  "\xe2\x94\x82"    /* │ */
#define G_TT "\xe2\x94\xac"    /* ┬ */
#define G_BT "\xe2\x94\xb4"    /* ┴ */
#define G_LT "\xe2\x94\x9c"    /* ├ */
#define G_RT "\xe2\x94\xa4"    /* ┤ */
#define G_X  "\xe2\x94\xbc"    /* ┼ */
#define G_HDOTS "\xe2\x80\xa6" /* … */
#define G_VDOTS "\xe2\x94\x86" /* ┆ */

/* ===== Table formatter helpers ===== */

static void fmt_centered(fmt_buf_t* b, const char* s, int32_t slen, int32_t width) {
    int32_t left  = (width - slen) / 2;
    int32_t right = width - slen - left;
    for (int32_t i = 0; i < left; i++)  fmt_putc(b, ' ');
    fmt_putn(b, s, slen);
    for (int32_t i = 0; i < right; i++) fmt_putc(b, ' ');
}

/* Maximum pre-formatted cells: FMT_TABLE_MAX_WIDTH * FMT_TABLE_MAX_HEIGHT = 200 */
#define FMT_CELL_BUF_SIZE 64

typedef struct {
    char    str[FMT_CELL_BUF_SIZE];
    int32_t len;
} fmt_cell_t;

static void fmt_table(fmt_buf_t* b, ray_t* tbl, int mode) {
    int64_t ncols = ray_table_ncols(tbl);
    int64_t nrows = ray_table_nrows(tbl);

    /* Compact mode: round-trippable (table [names] (list col1 col2 ...)) */
    if (mode == 0) {
        fmt_puts(b, "(table [");
        for (int64_t i = 0; i < ncols; i++) {
            if (i > 0) fmt_putc(b, ' ');
            int64_t name_id = ray_table_col_name(tbl, i);
            ray_t* name_str = ray_sym_str(name_id);
            if (name_str && !RAY_IS_ERR(name_str)) {
                fmt_putn(b, ray_str_ptr(name_str), (int32_t)ray_str_len(name_str));
                ray_release(name_str);
            }
        }
        fmt_puts(b, "] (list ");
        for (int64_t i = 0; i < ncols; i++) {
            if (i > 0) fmt_putc(b, ' ');
            ray_t* col = ray_table_get_col_idx(tbl, i);
            if (col) {
                fmt_obj(b, col, mode);
            }
        }
        fmt_puts(b, "))");
        return;
    }

    /* Full mode (1) and show mode (2) */
    int64_t table_width  = ncols;
    int64_t table_height = nrows;

    if (mode == 1) {
        if (table_width > FMT_TABLE_MAX_WIDTH)
            table_width = FMT_TABLE_MAX_WIDTH;
        if (table_height > FMT_TABLE_MAX_HEIGHT)
            table_height = FMT_TABLE_MAX_HEIGHT;
    }

    if (table_width == 0) {
        fmt_puts(b, "<table>");
        return;
    }

    bool has_hidden_cols = (table_width < ncols);
    bool has_hidden_rows = (table_height < nrows);

    /* Allocate metadata arrays.  For mode 1 they fit on the stack
     * (max 10 cols x 20 rows).  For mode 2 we heap-allocate. */
    bool heap_alloc = (table_width > FMT_TABLE_MAX_WIDTH ||
                       table_height > FMT_TABLE_MAX_HEIGHT);

    int32_t     col_widths_stack[FMT_TABLE_MAX_WIDTH];
    const char* col_names_stack[FMT_TABLE_MAX_WIDTH];
    int32_t     col_name_lens_stack[FMT_TABLE_MAX_WIDTH];
    const char* col_types_stack[FMT_TABLE_MAX_WIDTH];
    int32_t     col_type_lens_stack[FMT_TABLE_MAX_WIDTH];
    ray_t*      name_refs_stack[FMT_TABLE_MAX_WIDTH];
    fmt_cell_t  cells_stack[FMT_TABLE_MAX_WIDTH * FMT_TABLE_MAX_HEIGHT];

    /* Heap-backed pointers (NULL when using stack) */
    ray_t* heap_widths_blk = NULL;
    ray_t* heap_names_blk  = NULL;
    ray_t* heap_nlen_blk   = NULL;
    ray_t* heap_types_blk  = NULL;
    ray_t* heap_tlen_blk   = NULL;
    ray_t* heap_refs_blk   = NULL;
    ray_t* heap_cells_blk  = NULL;

    int32_t*     col_widths;
    const char** col_names;
    int32_t*     col_name_lens;
    const char** col_types;
    int32_t*     col_type_lens;
    ray_t**      name_refs;
    fmt_cell_t*  cells;

    if (!heap_alloc) {
        col_widths    = col_widths_stack;
        col_names     = col_names_stack;
        col_name_lens = col_name_lens_stack;
        col_types     = col_types_stack;
        col_type_lens = col_type_lens_stack;
        name_refs     = name_refs_stack;
        cells         = cells_stack;
    } else {
        heap_widths_blk = ray_alloc((size_t)(table_width * (int64_t)sizeof(int32_t)));
        heap_names_blk  = ray_alloc((size_t)(table_width * (int64_t)sizeof(const char*)));
        heap_nlen_blk   = ray_alloc((size_t)(table_width * (int64_t)sizeof(int32_t)));
        heap_types_blk  = ray_alloc((size_t)(table_width * (int64_t)sizeof(const char*)));
        heap_tlen_blk   = ray_alloc((size_t)(table_width * (int64_t)sizeof(int32_t)));
        heap_refs_blk   = ray_alloc((size_t)(table_width * (int64_t)sizeof(ray_t*)));
        heap_cells_blk  = ray_alloc((size_t)(table_width * table_height * (int64_t)sizeof(fmt_cell_t)));

        col_widths    = (int32_t*)ray_data(heap_widths_blk);
        col_names     = (const char**)ray_data(heap_names_blk);
        col_name_lens = (int32_t*)ray_data(heap_nlen_blk);
        col_types     = (const char**)ray_data(heap_types_blk);
        col_type_lens = (int32_t*)ray_data(heap_tlen_blk);
        name_refs     = (ray_t**)ray_data(heap_refs_blk);
        cells         = (fmt_cell_t*)ray_data(heap_cells_blk);
    }

    /* Pre-format cells and calculate column widths */
    for (int64_t ci = 0; ci < table_width; ci++) {
        /* Column name */
        int64_t name_id = ray_table_col_name(tbl, ci);
        ray_t* name_str = ray_sym_str(name_id);
        name_refs[ci] = name_str;
        if (name_str && !RAY_IS_ERR(name_str)) {
            col_names[ci]     = ray_str_ptr(name_str);
            col_name_lens[ci] = (int32_t)ray_str_len(name_str);
        } else {
            col_names[ci]     = "?";
            col_name_lens[ci] = 1;
            name_refs[ci]     = NULL;
        }

        /* Column type */
        ray_t* col_vec = ray_table_get_col_idx(tbl, ci);
        const char* tname = ray_type_name(col_vec ? col_vec->type : 0);
        col_types[ci]     = tname;
        col_type_lens[ci] = (int32_t)strlen(tname);

        /* Start with max of name and type lengths */
        int32_t max_w = col_name_lens[ci];
        if (col_type_lens[ci] > max_w) max_w = col_type_lens[ci];

        int64_t col_len = col_vec ? ray_len(col_vec) : 0;

        /* Format first half (head rows) */
        int64_t half = table_height / 2;
        for (int64_t ri = 0; ri < half; ri++) {
            fmt_cell_t* cell = &cells[ci * table_height + ri];
            if (ri < col_len) {
                fmt_buf_t tmp;
                fmt_init(&tmp);
                fmt_raw_elem(&tmp, col_vec, ri);
                int32_t clen = tmp.len < FMT_CELL_BUF_SIZE - 1 ? tmp.len : FMT_CELL_BUF_SIZE - 1;
                memcpy(cell->str, tmp.buf, (size_t)clen);
                cell->str[clen] = '\0';
                cell->len = clen;
                fmt_destroy(&tmp);
            } else {
                memcpy(cell->str, "NA", 3);
                cell->len = 2;
            }
            if (cell->len > max_w) max_w = cell->len;
        }

        /* Format second half (tail rows) */
        for (int64_t ri = half; ri < table_height; ri++) {
            fmt_cell_t* cell = &cells[ci * table_height + ri];
            int64_t src_idx;
            if (table_height == col_len || !has_hidden_rows) {
                src_idx = ri;
            } else {
                src_idx = col_len - table_height + ri;
            }
            if (src_idx >= 0 && src_idx < col_len) {
                fmt_buf_t tmp;
                fmt_init(&tmp);
                fmt_raw_elem(&tmp, col_vec, src_idx);
                int32_t clen = tmp.len < FMT_CELL_BUF_SIZE - 1 ? tmp.len : FMT_CELL_BUF_SIZE - 1;
                memcpy(cell->str, tmp.buf, (size_t)clen);
                cell->str[clen] = '\0';
                cell->len = clen;
                fmt_destroy(&tmp);
            } else {
                memcpy(cell->str, "NA", 3);
                cell->len = 2;
            }
            if (cell->len > max_w) max_w = cell->len;
        }

        col_widths[ci] = max_w + 2; /* +2 for padding (1 space each side) */
    }

    /* Calculate total width (sum of col widths + separators between columns) */
    int32_t total_width = 0;
    for (int64_t ci = 0; ci < table_width; ci++)
        total_width += col_widths[ci];
    total_width += (int32_t)(table_width - 1); /* separators between columns */

    /* Format footer to check if we need to widen the last column */
    char footer[128];
    int footer_len = snprintf(footer, sizeof(footer),
        " %" PRId64 " rows (%" PRId64 " shown) %" PRId64 " columns (%" PRId64 " shown)",
        nrows, table_height, ncols, table_width);

    if (total_width < footer_len) {
        col_widths[table_width - 1] += footer_len - total_width;
        total_width = footer_len;
    }

    /* Extra width for hidden columns indicator */
    if (has_hidden_cols)
        total_width += 4; /* "───┐" or " … │" */

    /* === Render === */

    /* 1. Top border: ┌──┬──┐ */
    fmt_puts(b, G_TL);
    for (int64_t ci = 0; ci < table_width; ci++) {
        for (int32_t j = 0; j < col_widths[ci]; j++)
            fmt_puts(b, G_H);
        if (ci < table_width - 1 || has_hidden_cols)
            fmt_puts(b, G_TT);
        else
            fmt_puts(b, G_TR);
    }
    if (has_hidden_cols) {
        fmt_puts(b, G_H G_H G_H G_TR);
    }

    /* 2. Header row: │ name │ (centered) */
    fmt_putc(b, '\n');
    fmt_puts(b, G_V);
    for (int64_t ci = 0; ci < table_width; ci++) {
        fmt_centered(b, col_names[ci], col_name_lens[ci], col_widths[ci]);
        fmt_puts(b, G_V);
    }
    if (has_hidden_cols) {
        fmt_puts(b, " " G_HDOTS " " G_V);
    }

    /* 3. Type row: │ type │ (centered) */
    fmt_putc(b, '\n');
    fmt_puts(b, G_V);
    for (int64_t ci = 0; ci < table_width; ci++) {
        fmt_centered(b, col_types[ci], col_type_lens[ci], col_widths[ci]);
        fmt_puts(b, G_V);
    }
    if (has_hidden_cols) {
        fmt_puts(b, " " G_HDOTS " " G_V);
    }

    /* 4. Separator: ├──┼──┤ */
    fmt_putc(b, '\n');
    fmt_puts(b, G_LT);
    for (int64_t ci = 0; ci < table_width; ci++) {
        for (int32_t j = 0; j < col_widths[ci]; j++)
            fmt_puts(b, G_H);
        if (ci < table_width - 1 || has_hidden_cols)
            fmt_puts(b, G_X);
        else
            fmt_puts(b, G_RT);
    }
    if (has_hidden_cols) {
        fmt_puts(b, G_H G_H G_H G_RT);
    }

    /* 5. Data rows */
    int64_t half = table_height / 2;
    for (int64_t ri = 0; ri < table_height; ri++) {
        fmt_putc(b, '\n');

        /* 6. Truncation indicator row between head and tail */
        if (has_hidden_rows && ri == half) {
            fmt_puts(b, G_VDOTS);
            for (int64_t ci = 0; ci < table_width; ci++) {
                /* Center the ellipsis (3 bytes, 1 display char) */
                int32_t left  = (col_widths[ci] - 1) / 2;
                int32_t right = col_widths[ci] - 1 - left;
                for (int32_t p = 0; p < left; p++)  fmt_putc(b, ' ');
                fmt_puts(b, G_HDOTS);
                for (int32_t p = 0; p < right; p++) fmt_putc(b, ' ');
                fmt_puts(b, G_VDOTS);
            }
            if (has_hidden_cols) {
                fmt_puts(b, " " G_HDOTS " " G_VDOTS);
            }
            fmt_putc(b, '\n');
        }

        /* Data row: │ val │ (left-aligned with 1-space padding) */
        fmt_puts(b, G_V);
        for (int64_t ci = 0; ci < table_width; ci++) {
            fmt_cell_t* cell = &cells[ci * table_height + ri];
            fmt_putc(b, ' ');
            fmt_putn(b, cell->str, cell->len);
            int32_t pad = col_widths[ci] - cell->len - 1;
            for (int32_t p = 0; p < pad; p++)
                fmt_putc(b, ' ');
            fmt_puts(b, G_V);
        }
        if (has_hidden_cols) {
            fmt_puts(b, " " G_HDOTS " " G_V);
        }
    }

    /* 7. Bottom border (separator before footer): ├──┴──┤ */
    fmt_putc(b, '\n');
    fmt_puts(b, G_LT);
    for (int64_t ci = 0; ci < table_width; ci++) {
        for (int32_t j = 0; j < col_widths[ci]; j++)
            fmt_puts(b, G_H);
        if (ci < table_width - 1 || has_hidden_cols)
            fmt_puts(b, G_BT);
        else
            fmt_puts(b, G_RT);
    }
    if (has_hidden_cols) {
        fmt_puts(b, G_H G_H G_H G_RT);
    }

    /* 8. Footer row: │ N rows (M shown) C columns (K shown) │ */
    fmt_putc(b, '\n');
    fmt_puts(b, G_V);
    fmt_putn(b, footer, footer_len);
    for (int32_t i = footer_len; i < total_width; i++)
        fmt_putc(b, ' ');
    fmt_puts(b, G_V);

    /* Final bottom border: └───┘ */
    fmt_putc(b, '\n');
    fmt_puts(b, G_BL);
    for (int32_t i = 0; i < total_width; i++)
        fmt_puts(b, G_H);
    fmt_puts(b, G_BR);

    /* Release name string refs */
    for (int64_t ci = 0; ci < table_width; ci++) {
        if (name_refs[ci]) ray_release(name_refs[ci]);
    }

    /* Free heap allocations if used */
    if (heap_alloc) {
        ray_free(heap_widths_blk);
        ray_free(heap_names_blk);
        ray_free(heap_nlen_blk);
        ray_free(heap_types_blk);
        ray_free(heap_tlen_blk);
        ray_free(heap_refs_blk);
        ray_free(heap_cells_blk);
    }
}

/* ===== Core dispatch ===== */

static void fmt_obj(fmt_buf_t* b, ray_t* obj, int mode) {
    if (!obj || RAY_IS_NULL(obj)) { fmt_puts(b, "null"); return; }
    if (RAY_IS_ERR(obj)) {
        char code[8] = {0};
        memcpy(code, obj->sdata, obj->slen < 7 ? obj->slen : 7);
        fmt_puts(b, "error: ");
        fmt_puts(b, code);
        return;
    }

    int8_t type = obj->type;
    if (type < 0) {
        /* Typed null atom: null bit set → display as 0Nx */
        if (RAY_ATOM_IS_NULL(obj)) {
            fmt_puts(b, null_literal(-type));
            return;
        }
        /* Atom: type is negated */
        switch (-type) {
        case RAY_BOOL: fmt_bool(b, obj->b8); break;
        case RAY_U8:   fmt_u8(b, obj->u8); break;

        case RAY_I16:  fmt_i16(b, obj->i16); break;
        case RAY_I32:  fmt_i32(b, obj->i32); break;
        case RAY_I64:  fmt_i64(b, obj->i64); break;
        case RAY_F32:       fmt_f32(b, (float)obj->f64); break;
        case RAY_F64:       fmt_f64(b, obj->f64); break;
        case RAY_DATE:      fmt_date(b, obj->i32); break;
        case RAY_TIME:      fmt_time(b, obj->i32); break;
        case RAY_TIMESTAMP: fmt_timestamp(b, obj->i64); break;
        case RAY_SYM:  fmt_sym(b, obj->i64); break;
        case RAY_STR:  fmt_str_atom(b, obj, mode > 0); break;
        case RAY_GUID: fmt_guid(b, obj->obj ? (const uint8_t*)ray_data(obj->obj) : (const uint8_t*)ray_data(obj)); break;
        default:       fmt_puts(b, "?"); break;
        }
    } else if (ray_is_vec(obj)) {
        int limit = (mode == 1) ? g_row_width : -1;
        fmt_vector(b, obj, limit);
    } else if (type == RAY_LIST) {
        fmt_list(b, obj, mode);
    } else if (type == RAY_TABLE) {
        fmt_table(b, obj, mode);
    } else if (type == RAY_DICT) {
        fmt_dict(b, obj, mode);
    } else if (type == RAY_LAMBDA) {
        fmt_puts(b, "lambda");
    } else if (type == RAY_UNARY || type == RAY_BINARY || type == RAY_VARY) {
        /* Render function objects with angle brackets so a fn is
         * visually distinct from a sym or string.  Without them,
         * `.os` printed as `{getenv:.os.getenv …}` — looked like
         * a dict of sym self-references.  Now it reads
         * `{getenv:<.os.getenv> …}`. */
        const char* name = ray_fn_name(obj);
        if (name[0]) { fmt_puts(b, "<"); fmt_puts(b, name); fmt_puts(b, ">"); }
        else fmt_puts(b, type == RAY_UNARY ? "<builtin/1>" :
                         type == RAY_BINARY ? "<builtin/2>" : "<builtin/n>");
    } else if (type == RAY_LAZY) {
        ray_t* concrete = ray_lazy_materialize(obj);
        fmt_obj(b, concrete, mode);
        return;
    } else {
        fmt_printf(b, "<%s>", ray_type_name(type));
    }
}

ray_t* ray_fmt(ray_t* obj, int mode) {
    fmt_buf_t b;
    fmt_init(&b);
    fmt_obj(&b, obj, mode);
    return fmt_to_str(&b);
}

void ray_fmt_print(FILE* fp, ray_t* obj, int mode) {
    ray_t* s = ray_fmt(obj, mode);
    if (s) {
        fwrite(ray_str_ptr(s), 1, ray_str_len(s), fp);
        ray_release(s);
    }
}
