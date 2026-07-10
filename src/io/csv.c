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

/* ============================================================================
 * csv.c — Fast parallel CSV reader
 *
 * Design:
 *   1. mmap for zero-copy file access
 *   2. memchr-based newline scan for row offset discovery
 *   3. Single-pass: sample-based type inference, then parallel value parsing
 *   4. Inline integer/float parsers (bypass strtoll/strtod overhead)
 *   5. Parallel row parsing via ray_pool_dispatch
 *   6. Per-worker local sym tables, merged post-parse on main thread
 * ============================================================================ */

#if defined(__linux__)
  #define _GNU_SOURCE
#endif

#include "csv.h"
#include "mem/heap.h"
#include "mem/sys.h"
#include "core/numparse.h"
#include "core/pool.h"
#include "core/platform.h"   /* ray_vm_map_fd_ro / ray_vm_unmap_file (tracked) */
#include "lang/format.h"
#include "ops/hash.h"
#include "ops/idxop.h"      /* attach per-chunk zone index after load */
#include "store/col.h"
#include "store/fileio.h"
#include "store/splay.h"
#include "table/sym.h"
#include "table/domain.h"
#include "vec/str.h"

#include <inttypes.h>
#include <math.h>
#include <stdarg.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <fcntl.h>
#ifndef RAY_OS_WINDOWS
#include <unistd.h>
#endif
#include <sys/mman.h>

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */

#define CSV_MAX_COLS      256
#define CSV_SAMPLE_ROWS   4096
#define CSV_STR_DISTINCT_MIN 256
#define CSV_PART_ROWS_DEFAULT 1000000

/* --------------------------------------------------------------------------
 * mmap flags
 * -------------------------------------------------------------------------- */

#define MMAP_FLAGS MAP_PRIVATE

/* --------------------------------------------------------------------------
 * Scratch memory helpers (same pattern as exec.c).
 * Uses ray_alloc/ray_free (buddy allocator) instead of malloc/free.
 * -------------------------------------------------------------------------- */

static inline void* scratch_alloc(ray_t** hdr_out, size_t nbytes) {
    ray_t* h = ray_alloc(nbytes);
    if (!h) { *hdr_out = NULL; return NULL; }
    *hdr_out = h;
    return ray_data(h);
}

static inline void* scratch_realloc(ray_t** hdr_out, size_t old_bytes, size_t new_bytes) {
    ray_t* old_h = *hdr_out;
    ray_t* new_h = ray_alloc(new_bytes);
    if (!new_h) return NULL;
    void* new_p = ray_data(new_h);
    if (old_h) {
        memcpy(new_p, ray_data(old_h), old_bytes < new_bytes ? old_bytes : new_bytes);
        ray_free(old_h);
    }
    *hdr_out = new_h;
    return new_p;
}

static inline void* scratch_calloc(ray_t** hdr_out, size_t nbytes) {
    void* p = scratch_alloc(hdr_out, nbytes);
    if (p) memset(p, 0, nbytes);
    return p;
}

static inline void scratch_free(ray_t* hdr) {
    if (hdr) ray_free(hdr);
}

/* Hash uses wyhash from ops/hash.h (ray_hash_bytes) — much faster than FNV-1a
 * for short strings typical in CSV columns. */

/* String reference — raw pointer into mmap'd buffer + length.
 * Used during parse phase; interned into sym table after parse. */
typedef struct {
    const char* ptr;
    uint32_t    len;
} csv_strref_t;

RAY_INLINE const char* scan_field(const char* p, const char* buf_end,
                                  char delimiter,
                                  const char** out, size_t* out_len,
                                  char* esc_buf, char** dyn_esc);

/* --------------------------------------------------------------------------
 * Type inference
 * -------------------------------------------------------------------------- */

/* csv_type_t enum (incl. CSV_TYPE_AUTO) is defined in csv.h */

/* Narrowest integer width holding [min,max], with I16 as the floor.
 * U8 renders as hex (e.g. 0x1b) and BOOL renders in bool form, so INT
 * columns — which are decimal integers — must never resolve narrower than
 * I16.  Nullable columns use a signed sentinel (NULL_Iw == INT_MIN for that
 * width); data values equal to the sentinel force widening to the next tier.
 * min>max means no finite values (empty/all-null) -> safe I64. */
csv_type_t csv_resolve_int_width(int64_t min, int64_t max, bool has_null) {
    if (min > max) return CSV_TYPE_I64;          /* empty/all-null — safe default */
    if (has_null) {
        if (min > NULL_I16 && max <= INT16_MAX) return CSV_TYPE_I16;
        if (min > NULL_I32 && max <= INT32_MAX) return CSV_TYPE_I32;
        return CSV_TYPE_I64;
    }
    if (min >= INT16_MIN && max <= INT16_MAX) return CSV_TYPE_I16;
    if (min >= INT32_MIN && max <= INT32_MAX) return CSV_TYPE_I32;
    return CSV_TYPE_I64;
}

static csv_type_t detect_type(const char* f, size_t len) {
    if (len == 0) return CSV_TYPE_UNKNOWN;

    /* Common null sentinel strings → UNKNOWN (will become NULL) */
    if ((len == 3 && (memcmp(f, "N/A", 3) == 0 || memcmp(f, "n/a", 3) == 0)) ||
        (len == 2 && (memcmp(f, "NA", 2) == 0 || memcmp(f, "na", 2) == 0)) ||
        (len == 4 && (memcmp(f, "null", 4) == 0 || memcmp(f, "NULL", 4) == 0 ||
                      memcmp(f, "None", 4) == 0 || memcmp(f, "none", 4) == 0)) ||
        (len == 1 && f[0] == '.'))  /* bare dot — not a valid value */
        return CSV_TYPE_UNKNOWN;

    /* NaN/Inf literals → float */
    if (len == 3) {
        if ((f[0]=='n'||f[0]=='N') && (f[1]=='a'||f[1]=='A') && (f[2]=='n'||f[2]=='N'))
            return CSV_TYPE_F64;
        if ((f[0]=='i'||f[0]=='I') && (f[1]=='n'||f[1]=='N') && (f[2]=='f'||f[2]=='F'))
            return CSV_TYPE_F64;
    }
    if ((len == 4 && (f[0]=='+' || f[0]=='-')) &&
        (f[1]=='i'||f[1]=='I') && (f[2]=='n'||f[2]=='N') && (f[3]=='f'||f[3]=='F'))
        return CSV_TYPE_F64;

    /* Boolean */
    if ((len == 4 && memcmp(f, "true", 4) == 0) ||
        (len == 5 && memcmp(f, "false", 5) == 0) ||
        (len == 4 && memcmp(f, "TRUE", 4) == 0) ||
        (len == 5 && memcmp(f, "FALSE", 5) == 0))
        return CSV_TYPE_BOOL;

    /* Numeric scan */
    const char* p = f;
    const char* end = f + len;
    if (*p == '-' || *p == '+') p++;
    bool has_dot = false, has_e = false, has_digit = false;
    while (p < end) {
        unsigned char c = (unsigned char)*p;
        if (c >= '0' && c <= '9') { has_digit = true; p++; continue; }
        if (c == '.' && !has_dot) { has_dot = true; p++; continue; }
        if ((c == 'e' || c == 'E') && !has_e) {
            has_e = true; p++;
            if (p < end && (*p == '-' || *p == '+')) p++;
            continue;
        }
        break;
    }
    if (p == end && has_digit) {
        if (!has_dot && !has_e) return CSV_TYPE_I64;
        return CSV_TYPE_F64;
    }

    /* Date: YYYY-MM-DD (exactly 10 chars) or Timestamp: YYYY-MM-DD{T| }HH:MM:SS */
    if (len >= 10 && f[4] == '-' && f[7] == '-') {
        bool is_date = true;
        for (int i = 0; i < 10; i++) {
            if (i == 4 || i == 7) continue;
            if ((unsigned)(f[i] - '0') > 9) { is_date = false; break; }
        }
        if (is_date) {
            if (len == 10) return CSV_TYPE_DATE;
            if (len >= 19 && (f[10] == 'T' || f[10] == ' ') &&
                f[13] == ':' && f[16] == ':') {
                const int tp[] = {11,12,14,15,17,18};
                bool is_ts = true;
                for (int i = 0; i < 6; i++) {
                    if ((unsigned)(f[tp[i]] - '0') > 9) { is_ts = false; break; }
                }
                if (is_ts) return CSV_TYPE_TIMESTAMP;
            }
        }
    }

    /* Time: HH:MM:SS[.ffffff] (at least 8 chars) */
    if (len >= 8 && f[2] == ':' && f[5] == ':') {
        const int tp[] = {0,1,3,4,6,7};
        bool is_time = true;
        for (int i = 0; i < 6; i++) {
            if ((unsigned)(f[tp[i]] - '0') > 9) { is_time = false; break; }
        }
        if (is_time) return CSV_TYPE_TIME;
    }

    return CSV_TYPE_STR;
}

static csv_type_t promote_csv_type(csv_type_t cur, csv_type_t obs) {
    if (cur == CSV_TYPE_UNKNOWN) return obs;
    if (obs == CSV_TYPE_UNKNOWN) return cur;
    if (cur == obs) return cur;
    if (cur == CSV_TYPE_STR || obs == CSV_TYPE_STR) return CSV_TYPE_STR;
    /* DATE + TIMESTAMP → TIMESTAMP */
    if ((cur == CSV_TYPE_DATE && obs == CSV_TYPE_TIMESTAMP) ||
        (cur == CSV_TYPE_TIMESTAMP && obs == CSV_TYPE_DATE))
        return CSV_TYPE_TIMESTAMP;
    /* Numeric promotion: BOOL ⊂ I64 ⊂ F64 (enum values 1 < 2 < 3) */
    if (cur <= CSV_TYPE_F64 && obs <= CSV_TYPE_F64) {
        if (cur == CSV_TYPE_F64 || obs == CSV_TYPE_F64) return CSV_TYPE_F64;
        if (cur == CSV_TYPE_I64 || obs == CSV_TYPE_I64) return CSV_TYPE_I64;
        return cur;
    }
    /* All other mixed types (e.g. DATE+I64, TIME+BOOL) → STR */
    return CSV_TYPE_STR;
}

static void csv_cardinality_note(uint32_t* hashes, uint16_t* lens,
                                 uint16_t* distinct, uint16_t* non_null,
                                 int col, const char* fld, size_t flen) {
    if (flen == 0) return;
    size_t base = (size_t)col * CSV_SAMPLE_ROWS;
    uint32_t h = (uint32_t)ray_hash_bytes(fld, flen);
    uint16_t l = flen > UINT16_MAX ? UINT16_MAX : (uint16_t)flen;
    for (uint16_t i = 0; i < distinct[col]; i++) {
        if (hashes[base + i] == h && lens[base + i] == l) {
            non_null[col]++;
            return;
        }
    }
    if (distinct[col] < CSV_SAMPLE_ROWS) {
        hashes[base + distinct[col]] = h;
        lens[base + distinct[col]] = l;
        distinct[col]++;
    }
    non_null[col]++;
}

static int8_t csv_resolve_inferred_type(csv_type_t t,
                                        uint16_t distinct,
                                        uint16_t non_null) {
    switch (t) {
        case CSV_TYPE_BOOL:      return RAY_BOOL;
        case CSV_TYPE_I64:       return RAY_I64;
        case CSV_TYPE_F64:       return RAY_F64;
        case CSV_TYPE_DATE:      return RAY_DATE;
        case CSV_TYPE_TIME:      return RAY_TIME;
        case CSV_TYPE_TIMESTAMP: return RAY_TIMESTAMP;
        case CSV_TYPE_GUID:      return RAY_GUID;
        case CSV_TYPE_STR:
            return (distinct >= CSV_STR_DISTINCT_MIN ||
                    (non_null >= 64 &&
                     (uint32_t)distinct * 100u >= (uint32_t)non_null * 80u))
                ? RAY_STR : RAY_SYM;
        default:
            return RAY_SYM;
    }
}

static bool csv_infer_types_from_offsets(const char* buf, const char* buf_end,
                                         const int64_t* row_offsets,
                                         int64_t n_rows, int ncols,
                                         char delimiter, char* esc_buf,
                                         int8_t* resolved_types) {
    csv_type_t col_types[CSV_MAX_COLS];
    memset(col_types, 0, (size_t)ncols * sizeof(csv_type_t));

    ray_t *hash_hdr = NULL, *lens_hdr = NULL;
    uint32_t* text_hashes = (uint32_t*)scratch_calloc(&hash_hdr,
        (size_t)ncols * CSV_SAMPLE_ROWS * sizeof(uint32_t));
    uint16_t* text_lens = (uint16_t*)scratch_calloc(&lens_hdr,
        (size_t)ncols * CSV_SAMPLE_ROWS * sizeof(uint16_t));
    uint16_t text_distinct[CSV_MAX_COLS] = {0};
    uint16_t text_non_null[CSV_MAX_COLS] = {0};

    if (!text_hashes || !text_lens) {
        scratch_free(hash_hdr);
        scratch_free(lens_hdr);
        return false;
    }

    int64_t sample_n = n_rows < CSV_SAMPLE_ROWS ? n_rows : CSV_SAMPLE_ROWS;
    for (int64_t si = 0; si < sample_n; si++) {
        int64_t r = si;
        if (sample_n > 1 && sample_n < n_rows)
            r = (si * (n_rows - 1)) / (sample_n - 1);
        const char* rp = buf + row_offsets[r];
        for (int c = 0; c < ncols; c++) {
            const char* fld;
            size_t flen;
            char* dyn_esc = NULL;
            rp = scan_field(rp, buf_end, delimiter, &fld, &flen, esc_buf, &dyn_esc);
            csv_type_t t = detect_type(fld, flen);
            if (t == CSV_TYPE_STR)
                csv_cardinality_note(text_hashes, text_lens,
                                     text_distinct, text_non_null,
                                     c, fld, flen);
            if (dyn_esc) ray_sys_free(dyn_esc);
            col_types[c] = promote_csv_type(col_types[c], t);
        }
    }

    for (int c = 0; c < ncols; c++)
        resolved_types[c] = csv_resolve_inferred_type(
            col_types[c], text_distinct[c], text_non_null[c]);

    scratch_free(hash_hdr);
    scratch_free(lens_hdr);
    return true;
}

/* --------------------------------------------------------------------------
 * Zero-copy field scanner
 *
 * Returns pointer past the field's trailing delimiter (or at newline/end).
 * Sets *out and *out_len to the field content. For unquoted fields, *out
 * points directly into the mmap buffer. For quoted fields with escaped
 * quotes, content is unescaped into esc_buf.
 * -------------------------------------------------------------------------- */

static const char* scan_field_quoted(const char* p, const char* buf_end,
                                     char delim,
                                     const char** out, size_t* out_len,
                                     char* esc_buf, char** dyn_esc) {
    p++; /* skip opening quote */
    const char* fld_start = p;
    bool has_escape = false;

    while (p < buf_end) {
        if (*p == '"') {
            if (p + 1 < buf_end && *(p + 1) == '"') {
                has_escape = true;
                p += 2;
            } else {
                break; /* closing quote */
            }
        } else {
            p++;
        }
    }
    size_t raw_len = (size_t)(p - fld_start);
    if (p < buf_end && *p == '"') p++; /* skip closing quote */

    if (has_escape) {
        /* raw_len >= output length (quotes are collapsed); no overflow. */
        char* dest = esc_buf;
        if (RAY_UNLIKELY(raw_len > 8192)) {
            /* Field too large for stack buffer — dynamically allocate */
            dest = (char*)ray_sys_alloc(raw_len);
            if (!dest) {
                /* OOM: fall back to raw (quotes remain) */
                *out = fld_start;
                *out_len = raw_len;
                goto advance;
            }
            *dyn_esc = dest;
        }
        size_t olen = 0;
        for (const char* s = fld_start; s < fld_start + raw_len; s++) {
            if (*s == '"' && s + 1 < fld_start + raw_len && *(s + 1) == '"') {
                dest[olen++] = '"';
                s++;
            } else {
                dest[olen++] = *s;
            }
        }
        *out = dest;
        *out_len = olen;
    } else {
        *out = fld_start;
        *out_len = raw_len;
    }

advance:
    /* Advance past delimiter */
    if (p < buf_end && *p == delim) p++;
    /* Don't advance past newline — caller handles row boundaries */
    return p;
}

RAY_INLINE const char* scan_field(const char* p, const char* buf_end,
                                  char delim,
                                  const char** out, size_t* out_len,
                                  char* esc_buf, char** dyn_esc) {
    if (RAY_UNLIKELY(p >= buf_end)) {
        *out = p;
        *out_len = 0;
        return p;
    }

    if (RAY_LIKELY(*p != '"')) {
        /* Unquoted field — fast path */
        const char* s = p;
        while (p < buf_end && *p != delim && *p != '\n' && *p != '\r') p++;
        *out = s;
        *out_len = (size_t)(p - s);
        if (p < buf_end && *p == delim) return p + 1;
        return p;
    }

    return scan_field_quoted(p, buf_end, delim, out, out_len, esc_buf, dyn_esc);
}

/* --------------------------------------------------------------------------
 * Numeric field parsers — thin wrappers over core/numparse with the
 * CSV semantics that the *entire* field must be consumed; otherwise
 * the cell is null.
 * -------------------------------------------------------------------------- */

RAY_INLINE int64_t fast_i64(const char* p, size_t len, bool* is_null) {
    int64_t v = 0;
    size_t n = ray_parse_i64(p, len, &v);
    *is_null = (n == 0 || n != len);
    return *is_null ? 0 : v;
}

RAY_INLINE double fast_f64(const char* p, size_t len, bool* is_null) {
    double v = 0.0;
    size_t n = ray_parse_f64(p, len, &v);
    /* STAGE 2 (single-null float model): ray_parse_f64 already canonicalizes
     * any non-finite parse ("inf"/"nan"/"1e400") to NULL_F64 (0Nf).  Treat
     * an unparseable cell OR a canonical 0Nf as null so the column's
     * HAS_NULLS / null bitmap is marked the same way as a blank cell — the
     * F64 domain is {finite} ∪ {0Nf}, and a 0Nf row reads as nil?.
     * `v != v` is the model's null test (true only for the NaN-bit 0Nf). */
    *is_null = (n == 0 || n != len || v != v);
    return *is_null ? NULL_F64 : v;
}

/* --------------------------------------------------------------------------
 * Fast inline date/time parsers
 *
 * DATE:      YYYY-MM-DD        → int32_t  (days since 2000-01-01)
 * TIME:      HH:MM:SS[.fff]    → int32_t  (milliseconds since midnight)
 * TIMESTAMP: YYYY-MM-DD{T| }HH:MM:SS[.ffffff] → int64_t (µs since 2000-01-01)
 *
 * Uses Howard Hinnant's civil-calendar algorithm (public domain) for the
 * date→days conversion — O(1), no tables, no branches.
 * -------------------------------------------------------------------------- */

RAY_INLINE int32_t civil_to_days(int y, int m, int d) {
    /* Shift Jan/Feb to months 10/11 of the previous year */
    if (m <= 2) { y--; m += 9; } else { m -= 3; }
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * m + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int32_t)(era * 146097 + doe - 719468 - 10957);
}

RAY_INLINE int32_t fast_date(const char* p, size_t len, bool* is_null) {
    if (RAY_UNLIKELY(len < 10)) { *is_null = true; return 0; }
    *is_null = false;
    int y = (p[0]-'0')*1000 + (p[1]-'0')*100 + (p[2]-'0')*10 + (p[3]-'0');
    int m = (p[5]-'0')*10 + (p[6]-'0');
    int d = (p[8]-'0')*10 + (p[9]-'0');
    if (RAY_UNLIKELY(m < 1 || m > 12 || d < 1 || d > 31)) { *is_null = true; return 0; }
    return civil_to_days(y, m, d);
}

/* TIME → int32_t milliseconds since midnight */
RAY_INLINE int32_t fast_time(const char* p, size_t len, bool* is_null) {
    if (RAY_UNLIKELY(len < 8)) { *is_null = true; return 0; }
    *is_null = false;
    int h  = (p[0]-'0')*10 + (p[1]-'0');
    int mi = (p[3]-'0')*10 + (p[4]-'0');
    int s  = (p[6]-'0')*10 + (p[7]-'0');
    if (RAY_UNLIKELY(h > 23 || mi > 59 || s > 59)) { *is_null = true; return 0; }
    int32_t ms = h * 3600000 + mi * 60000 + s * 1000;
    /* Fractional seconds → milliseconds */
    if (len > 8 && p[8] == '.') {
        int frac = 0, digits = 0;
        for (size_t i = 9; i < len && digits < 3; i++, digits++) {
            unsigned di = (unsigned char)p[i] - '0';
            if (di > 9) break;
            frac = frac * 10 + (int)di;
        }
        while (digits < 3) { frac *= 10; digits++; }
        ms += (int32_t)frac;
    }
    return ms;
}

/* Timestamp time component → int64_t nanoseconds.
 * RAY_TIMESTAMP is nanoseconds since 2000-01-01 (matching
 * src/lang/format.c:ts_to_parts and csv_write_timestamp).  Accept up
 * to 9 fractional digits; shorter fractions are right-padded with
 * zeros, longer ones are truncated. */
RAY_INLINE int64_t fast_time_ns(const char* p, size_t len, bool* is_null, size_t* consumed) {
    if (RAY_UNLIKELY(len < 8)) { *is_null = true; return 0; }
    *is_null = false;
    int h  = (p[0]-'0')*10 + (p[1]-'0');
    int mi = (p[3]-'0')*10 + (p[4]-'0');
    int s  = (p[6]-'0')*10 + (p[7]-'0');
    if (RAY_UNLIKELY(h > 23 || mi > 59 || s > 59)) { *is_null = true; return 0; }
    int64_t ns = (int64_t)h * 3600000000000LL + (int64_t)mi * 60000000000LL +
                 (int64_t)s * 1000000000LL;
    size_t used = 8;
    if (len > 8 && p[8] == '.') {
        int64_t frac = 0;
        int digits = 0;
        size_t i = 9;
        for (; i < len && digits < 9; i++, digits++) {
            unsigned di = (unsigned char)p[i] - '0';
            if (di > 9) break;
            frac = frac * 10 + (int64_t)di;
        }
        while (digits < 9) { frac *= 10; digits++; }
        ns += frac;
        used = i;  /* index of the first char past the fractional seconds */
    }
    if (consumed) *consumed = used;
    return ns;
}

/* Parse a trailing ISO-8601 UTC offset at p: 'Z'/'z' (== UTC) or
 * (+|-)HH[[:]MM].  On success sets *out_ns to the signed offset in
 * nanoseconds (to be SUBTRACTED from the local wall-clock value to get
 * UTC) and returns true.  Returns false if the suffix is not a
 * recognized offset (caller then leaves the value unadjusted, preserving
 * the prior behaviour of ignoring unrecognized trailing characters). */
RAY_INLINE bool parse_tz_offset(const char* p, size_t len, int64_t* out_ns) {
    if (len == 0) return false;
    if (p[0] == 'Z' || p[0] == 'z') { *out_ns = 0; return true; }
    int sign;
    if (p[0] == '+') sign = 1;
    else if (p[0] == '-') sign = -1;
    else return false;
    if (len < 3 || p[1] < '0' || p[1] > '9' || p[2] < '0' || p[2] > '9') return false;
    int hh = (p[1]-'0')*10 + (p[2]-'0');
    int mm = 0;
    size_t i = 3;
    if (len > i && p[i] == ':') i++;            /* optional ':' separator */
    if (len >= i + 2 && p[i] >= '0' && p[i] <= '9' && p[i+1] >= '0' && p[i+1] <= '9')
        mm = (p[i]-'0')*10 + (p[i+1]-'0');
    if (RAY_UNLIKELY(hh > 23 || mm > 59)) return false;
    *out_ns = (int64_t)sign * ((int64_t)hh * 3600 + (int64_t)mm * 60) * 1000000000LL;
    return true;
}

RAY_INLINE int64_t fast_timestamp(const char* p, size_t len, bool* is_null) {
    if (RAY_UNLIKELY(len < 19)) { *is_null = true; return 0; }
    *is_null = false;
    int32_t days = fast_date(p, 10, is_null);
    if (*is_null) return 0;
    bool time_null = false;
    size_t time_used = 8;
    int64_t time_ns = fast_time_ns(p + 11, len - 11, &time_null, &time_used);
    if (time_null) { *is_null = true; return 0; }
    const int64_t NS_PER_DAY = 86400000000000LL;
    int64_t result = (int64_t)days * NS_PER_DAY + time_ns;
    /* Optional trailing UTC offset (Z | ±HH:MM | ±HHMM | ±HH).  The common
     * no-offset case ends exactly at the time component (off == len), so
     * the hot path pays only this single predicted-not-taken bounds check. */
    size_t off = 11 + time_used;
    if (RAY_UNLIKELY(off < len)) {
        int64_t adj;
        if (parse_tz_offset(p + off, len - off, &adj)) result -= adj;
    }
    return result;
}

/* --------------------------------------------------------------------------
 * Null-aware boolean parser
 * -------------------------------------------------------------------------- */

RAY_INLINE uint8_t fast_bool(const char* s, size_t len, bool* is_null) {
    if (len == 0) { *is_null = true; return 0; }
    *is_null = false;
    if ((len == 4 && (memcmp(s, "true", 4) == 0 || memcmp(s, "TRUE", 4) == 0)) ||
        (len == 1 && s[0] == '1'))
        return 1;
    if ((len == 5 && (memcmp(s, "false", 5) == 0 || memcmp(s, "FALSE", 5) == 0)) ||
        (len == 1 && s[0] == '0'))
        return 0;
    *is_null = true;
    return 0;
}

/* --------------------------------------------------------------------------
 * GUID parser (mirrors csv_write_guid: 8-4-4-4-12 hex, 36 chars).
 * Writes 16 bytes to `dst`.  Sets *is_null on shape or hex mismatch.
 * -------------------------------------------------------------------------- */

RAY_INLINE int hex_nibble(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

RAY_INLINE void fast_guid(const char* p, size_t len, uint8_t* dst, bool* is_null) {
    if (RAY_UNLIKELY(len != 36 ||
                     p[8]  != '-' || p[13] != '-' ||
                     p[18] != '-' || p[23] != '-')) {
        *is_null = true;
        return;
    }
    /* Layout: bytes 0..3 from chars 0..7, then 4..5 from 9..12,
     * 6..7 from 14..17, 8..9 from 19..22, 10..15 from 24..35. */
    static const uint8_t pos[16] = { 0,2,4,6,  9,11, 14,16, 19,21, 24,26,28,30,32,34 };
    for (int i = 0; i < 16; i++) {
        int hi = hex_nibble((unsigned char)p[pos[i]]);
        int lo = hex_nibble((unsigned char)p[pos[i] + 1]);
        if (RAY_UNLIKELY((hi | lo) < 0)) { *is_null = true; return; }
        dst[i] = (uint8_t)((hi << 4) | lo);
    }
    *is_null = false;
}

/* --------------------------------------------------------------------------
 * Row offsets builder — memchr-accelerated
 *
 * Uses memchr (glibc: SIMD-accelerated ~15-20 GB/s) for newline scanning.
 * Fast path for quote-free files; falls back to byte-by-byte for quoted
 * fields with embedded newlines. Returns exact row count.
 *
 * Allocates offsets via scratch_alloc. Caller frees with scratch_free.
 * -------------------------------------------------------------------------- */

static int64_t build_row_offsets(const char* buf, size_t buf_size,
                                  size_t data_offset,
                                  int64_t** offsets_out, ray_t** hdr_out) {
    const char* p = buf + data_offset;
    const char* end = buf + buf_size;

    /* Do NOT skip leading blank lines: empty lines in the data section
     * are null rows (they were written out by write-csv for null-valued
     * single-column tables). Header-level whitespace is consumed by the
     * header parser before we reach data_offset. */
    if (p >= end) { *offsets_out = NULL; *hdr_out = NULL; return 0; }

    /* Estimate capacity: ~40 bytes per row + headroom.
     * 40 bytes/row is conservative for typical CSVs; realloc path handles
     * underestimates. */
    size_t remaining = (size_t)(end - p);
    int64_t est = (int64_t)(remaining / 40) + 16;
    ray_t* hdr = NULL;
    int64_t* offs = (int64_t*)scratch_alloc(&hdr, (size_t)est * sizeof(int64_t));
    if (!offs) { *offsets_out = NULL; *hdr_out = NULL; return 0; }

    int64_t n = 0;
    offs[n++] = (int64_t)(p - buf);

    /* Check if file has any quotes — determines fast vs slow path */
    bool has_quotes = (memchr(p, '"', remaining) != NULL);

    if (RAY_LIKELY(!has_quotes)) {
        /* Fast path: no quotes, use memchr for newlines.
         * Only scans for \n; pure \r line endings (old Mac) treated as single row.
         * Empty lines are preserved as rows (for NULL handling). */
        for (;;) {
            const char* nl = (const char*)memchr(p, '\n', (size_t)(end - p));
            if (!nl) break;
            p = nl + 1;
            /* Skip optional \r after \n (unusual \n\r endings) */
            if (p < end && *p == '\r') p++;
            if (p >= end) break;

            if (n >= est) {
                est *= 2;
                offs = (int64_t*)scratch_realloc(&hdr,
                    (size_t)n * sizeof(int64_t),
                    (size_t)est * sizeof(int64_t));
                if (!offs) { scratch_free(hdr); *offsets_out = NULL; *hdr_out = NULL; return 0; }
            }
            offs[n++] = (int64_t)(p - buf);
        }
    } else {
        /* Slow path: track quote parity, byte-by-byte.
         * Empty lines preserved as rows (for NULL handling). */
        bool in_quote = false;
        while (p < end) {
            char c = *p;
            if (c == '"') {
                in_quote = !in_quote;
                p++;
            } else if (!in_quote && (c == '\n' || c == '\r')) {
                if (c == '\r' && p + 1 < end && *(p + 1) == '\n') p++;
                p++;
                if (p < end) {
                    if (n >= est) {
                        est *= 2;
                        offs = (int64_t*)scratch_realloc(&hdr,
                            (size_t)n * sizeof(int64_t),
                            (size_t)est * sizeof(int64_t));
                        if (!offs) { scratch_free(hdr); *offsets_out = NULL; *hdr_out = NULL; return 0; }
                    }
                    offs[n++] = (int64_t)(p - buf);
                }
            } else {
                p++;
            }
        }
    }

    *offsets_out = offs;
    *hdr_out = hdr;
    return n;
}

static int64_t build_row_offsets_limited(const char* buf, size_t buf_size,
                                         size_t data_offset, int64_t max_rows,
                                         bool data_has_quotes,
                                         int64_t** offsets_out, ray_t** hdr_out,
                                         size_t* next_offset_out) {
    const char* p = buf + data_offset;
    const char* end = buf + buf_size;

    *offsets_out = NULL;
    *hdr_out = NULL;
    if (next_offset_out) *next_offset_out = data_offset;
    if (max_rows <= 0 || p >= end) return 0;

    size_t remaining = (size_t)(end - p);
    int64_t est = (int64_t)(remaining / 40) + 16;
    if (est < 1) est = 1;
    if (est > max_rows) est = max_rows;

    ray_t* hdr = NULL;
    int64_t* offs = (int64_t*)scratch_alloc(&hdr, (size_t)est * sizeof(int64_t));
    if (!offs) return 0;

    int64_t n = 0;
    offs[n++] = (int64_t)(p - buf);

    if (RAY_LIKELY(!data_has_quotes)) {
        for (;;) {
            const char* nl = (const char*)memchr(p, '\n', (size_t)(end - p));
            if (!nl) {
                p = end;
                break;
            }
            p = nl + 1;
            if (p < end && *p == '\r') p++;
            if (p >= end) break;
            if (n >= max_rows) break;
            if (n >= est) {
                int64_t new_est = est * 2;
                if (new_est > max_rows) new_est = max_rows;
                offs = (int64_t*)scratch_realloc(&hdr,
                    (size_t)n * sizeof(int64_t),
                    (size_t)new_est * sizeof(int64_t));
                if (!offs) {
                    scratch_free(hdr);
                    return 0;
                }
                est = new_est;
            }
            offs[n++] = (int64_t)(p - buf);
        }
    } else {
        bool in_quote = false;
        while (p < end) {
            char c = *p;
            if (c == '"') {
                in_quote = !in_quote;
                p++;
            } else if (!in_quote && (c == '\n' || c == '\r')) {
                if (c == '\r' && p + 1 < end && *(p + 1) == '\n') p++;
                p++;
                if (p >= end) break;
                if (n >= max_rows) break;
                if (n >= est) {
                    int64_t new_est = est * 2;
                    if (new_est > max_rows) new_est = max_rows;
                    offs = (int64_t*)scratch_realloc(&hdr,
                        (size_t)n * sizeof(int64_t),
                        (size_t)new_est * sizeof(int64_t));
                    if (!offs) {
                        scratch_free(hdr);
                        return 0;
                    }
                    est = new_est;
                }
                offs[n++] = (int64_t)(p - buf);
            } else {
                p++;
            }
        }
    }

    *offsets_out = offs;
    *hdr_out = hdr;
    if (next_offset_out) *next_offset_out = (size_t)(p - buf);
    return n;
}

/* --------------------------------------------------------------------------
 * Batch-intern string columns after parse.
 * Single-threaded — walks each string column, interns into global sym table,
 * writes sym IDs into the final uint32_t column.
 * -------------------------------------------------------------------------- */

static bool csv_intern_strings(csv_strref_t** str_refs, int n_cols,
                                const csv_type_t* col_types,
                                const int8_t* resolved_types,
                                void** col_data, int64_t n_rows,
                                int64_t* col_max_ids) {
    bool ok = true;

    /* CSV/TSV import policy for SYM columns: empty fields write the
     * canonical empty-symbol ID (always 0, reserved by ray_sym_init).
     * SYM columns carry no null bitmap by design — sym 0 is the
     * representation of "missing", "empty", and "absent" all at once.
     * The CSV format can't distinguish "missing field" from "empty
     * string" anyway, so collapsing them is the only deterministic
     * answer the parser can give.
     *
     * Sym 0 is guaranteed to exist by ray_sym_init; the intern call
     * here is a fast-path lookup that returns it without growing the
     * dictionary. */
    int64_t empty_sym_id = ray_sym_intern_prehashed(
        (uint32_t)ray_hash_bytes("", 0), "", 0);
    if (empty_sym_id < 0) empty_sym_id = 0;  /* shouldn't happen — sym 0 is reserved */

    for (int c = 0; c < n_cols; c++) {
        if (col_types[c] != CSV_TYPE_STR) continue;
        /* RAY_STR columns are materialized directly; skip sym interning. */
        if (resolved_types[c] == RAY_STR) continue;
        csv_strref_t* refs = str_refs[c];
        if (!refs) continue;
        uint32_t* ids = (uint32_t*)col_data[c];
        int64_t max_id = empty_sym_id;

        /* Pre-grow: upper bound is n_rows unique strings */
        uint32_t current = ray_sym_count();
        if (!ray_sym_ensure_cap(current + (uint32_t)(n_rows < UINT32_MAX ? n_rows : UINT32_MAX)))
            return false;  /* OOM: cannot grow sym table */

        for (int64_t r = 0; r < n_rows; r++) {
            if (refs[r].ptr == NULL) {
                /* Empty/missing field → sym 0 (the canonical empty
                 * symbol).  SYM columns are no-null by design. */
                ids[r] = (uint32_t)empty_sym_id;
                continue;
            }
            int64_t id = ray_sym_intern_no_split_unlocked(refs[r].ptr, refs[r].len);
            if (id < 0) { ok = false; id = 0; }
            ids[r] = (uint32_t)id;
            if (id > max_id) max_id = id;
        }
        if (col_max_ids) col_max_ids[c] = max_id;
    }
    return ok;
}

/* Free strref pointers that were heap-allocated for escaped CSV fields.
 * Any strref whose ptr falls outside the mmap buffer [buf, buf+buf_size)
 * was allocated by the parse loop and must be freed here. */
static void csv_free_escaped_strrefs(csv_strref_t** str_refs, int n_cols,
                                      const csv_type_t* col_types,
                                      int64_t n_rows,
                                      const char* buf, size_t buf_size) {
    const char* buf_end = buf + buf_size;
    for (int c = 0; c < n_cols; c++) {
        if (col_types[c] != CSV_TYPE_STR || !str_refs[c]) continue;
        for (int64_t r = 0; r < n_rows; r++) {
            const char* p = str_refs[c][r].ptr;
            if (p && (p < buf || p >= buf_end))
                ray_sys_free((void*)p);
        }
    }
}

/* Materialize RAY_STR columns from parsed strrefs. Two-pass so the per-column
 * string pool is sized exactly once — avoids the repeated realloc/COW path
 * that ray_str_vec_set would take for a freshly-owned vector. */
static bool csv_fill_str_cols(csv_strref_t** str_refs, int n_cols,
                              const int8_t* resolved_types,
                              ray_t** col_vecs, int64_t n_rows) {
    for (int c = 0; c < n_cols; c++) {
        if (resolved_types[c] != RAY_STR) continue;
        csv_strref_t* refs = str_refs[c];
        ray_t* vec = col_vecs[c];
        ray_str_t* dst = (ray_str_t*)ray_data(vec);

        /* ray_str_t.pool_off is u32 — the per-column pool is capped at 4 GiB.
         * Sum as u64 so the add itself can't wrap, then bail if the total
         * wouldn't fit in the u32 offset field. */
        uint64_t pool_bytes = 0;
        for (int64_t r = 0; r < n_rows; r++) {
            if (refs[r].ptr == NULL) continue;
            uint32_t l = refs[r].len;
            if (l > RAY_STR_INLINE_MAX) pool_bytes += l;
        }
        if (pool_bytes > UINT32_MAX) return false;

        if (pool_bytes > 0) {
            ray_t* pool = ray_alloc((size_t)pool_bytes);
            if (!pool || RAY_IS_ERR(pool)) return false;
            pool->type = RAY_U8;
            pool->len = 0;
            vec->str_pool = pool;
        }

        char* pool_base = vec->str_pool ? (char*)ray_data(vec->str_pool) : NULL;
        uint32_t pool_off = 0;

        for (int64_t r = 0; r < n_rows; r++) {
            memset(&dst[r], 0, sizeof(ray_str_t));
            if (refs[r].ptr == NULL) continue;
            const char* p = refs[r].ptr;
            uint32_t l = refs[r].len;
            dst[r].len = l;
            if (l <= RAY_STR_INLINE_MAX) {
                if (l > 0) memcpy(dst[r].data, p, l);
            } else {
                memcpy(dst[r].prefix, p, 4);
                dst[r].pool_off = pool_off;
                memcpy(pool_base + pool_off, p, l);
                pool_off += l;  /* cannot wrap: pool_bytes <= UINT32_MAX */
            }
        }
        if (vec->str_pool) vec->str_pool->len = (int64_t)pool_off;
    }
    return true;
}

/* --------------------------------------------------------------------------
 * Stage 9b helper: dispatch csv_fill_str_cols and csv_intern_strings on
 * separate threads when a pool is available.  They write to disjoint
 * column data, and intern_strings is the only one that touches the
 * global sym table (so it stays single-threaded; we just run it in
 * parallel with fill_str_cols).
 * -------------------------------------------------------------------------- */

typedef struct {
    csv_strref_t**    str_refs;
    int               n_cols;
    const csv_type_t* parse_types;
    const int8_t*     resolved_types;
    void**            col_data;
    ray_t**           col_vecs;
    int64_t           n_rows;
    int64_t*          sym_max_ids;
    bool              fill_ok;
    bool              intern_ok;
} csv_finalize_ctx_t;

static void csv_finalize_task(void* arg, uint32_t worker_id,
                              int64_t start, int64_t end_idx) {
    (void)worker_id; (void)end_idx;
    csv_finalize_ctx_t* ctx = (csv_finalize_ctx_t*)arg;
    if (start == 0) {
        ctx->fill_ok = csv_fill_str_cols(ctx->str_refs, ctx->n_cols,
            ctx->resolved_types, ctx->col_vecs, ctx->n_rows);
    } else {
        ctx->intern_ok = csv_intern_strings(ctx->str_refs, ctx->n_cols,
            ctx->parse_types, ctx->resolved_types, ctx->col_data,
            ctx->n_rows, ctx->sym_max_ids);
    }
}

/* --------------------------------------------------------------------------
 * Parallel parse context and callback
 * -------------------------------------------------------------------------- */

typedef struct {
    const char*       buf;
    size_t            buf_size;
    const int64_t*    row_offsets;
    int64_t           n_rows;
    int               n_cols;
    char              delim;
    const csv_type_t* col_types;
    const int8_t*     resolved_types;
    void**            col_data;     /* non-const: workers write parsed values into columns */
    csv_strref_t**    str_refs;     /* [n_cols] — strref arrays for string columns, NULL for others */
    bool*             worker_had_null; /* [n_workers * n_cols] */
} csv_par_ctx_t;

static void csv_parse_fn(void* arg, uint32_t worker_id,
                          int64_t start, int64_t end_row) {
    csv_par_ctx_t* ctx = (csv_par_ctx_t*)arg;
    char esc_buf[8192];
    const char* buf_end = ctx->buf + ctx->buf_size;
    bool* my_had_null = &ctx->worker_had_null[(size_t)worker_id * (size_t)ctx->n_cols];

    for (int64_t row = start; row < end_row; row++) {
        const char* p = ctx->buf + ctx->row_offsets[row];
        const char* row_end = (row + 1 < ctx->n_rows)
            ? ctx->buf + ctx->row_offsets[row + 1]
            : buf_end;

        for (int c = 0; c < ctx->n_cols; c++) {
            /* Guard: if past row boundary, fill remaining columns with defaults + null */
            if (p >= row_end) {
                for (; c < ctx->n_cols; c++) {
                    switch (ctx->col_types[c]) {
                        case CSV_TYPE_BOOL: ((uint8_t*)ctx->col_data[c])[row] = 0; break;
                        case CSV_TYPE_U8:   ((uint8_t*)ctx->col_data[c])[row] = 0; break;
                        case CSV_TYPE_I16:  ((int16_t*)ctx->col_data[c])[row] = NULL_I16; break;
                        case CSV_TYPE_I32:  ((int32_t*)ctx->col_data[c])[row] = NULL_I32; break;
                        case CSV_TYPE_I64:  ((int64_t*)ctx->col_data[c])[row] = NULL_I64; break;
                        case CSV_TYPE_F32:  ((float*)ctx->col_data[c])[row] = NULL_F32; break;
                        case CSV_TYPE_F64:  ((double*)ctx->col_data[c])[row] = NULL_F64; break;
                        case CSV_TYPE_DATE: ((int32_t*)ctx->col_data[c])[row] = NULL_I32; break;
                        case CSV_TYPE_TIME: ((int32_t*)ctx->col_data[c])[row] = NULL_I32; break;
                        case CSV_TYPE_TIMESTAMP:
                            ((int64_t*)ctx->col_data[c])[row] = NULL_I64; break;
                        case CSV_TYPE_GUID:
                            memset((uint8_t*)ctx->col_data[c] + (size_t)row * 16, 0, 16);
                            break;
                        case CSV_TYPE_STR:
                            ctx->str_refs[c][row].ptr = NULL;
                            ctx->str_refs[c][row].len = 0;
                            break;
                        default: break;
                    }
                    /* BOOL/U8 are non-nullable; empty cells store 0/false. */
                    if (ctx->col_types[c] != CSV_TYPE_BOOL &&
                        ctx->col_types[c] != CSV_TYPE_U8) {
                        my_had_null[c] = true;
                    }
                }
                break;
            }

            const char* fld;
            size_t flen;
            char* dyn_esc = NULL;
            p = scan_field(p, buf_end, ctx->delim, &fld, &flen, esc_buf, &dyn_esc);

            /* Strip trailing \r from last field of row */
            if (c == ctx->n_cols - 1 && flen > 0 && fld[flen - 1] == '\r')
                flen--;

            switch (ctx->col_types[c]) {
                case CSV_TYPE_BOOL: {
                    /* BOOL is non-nullable; fast_bool returns 0 for
                     * empty / unparseable input and we store it as-is. */
                    bool is_null;
                    uint8_t v = fast_bool(fld, flen, &is_null);
                    ((uint8_t*)ctx->col_data[c])[row] = v;
                    break;
                }
                case CSV_TYPE_I64: {
                    bool is_null;
                    int64_t v = fast_i64(fld, flen, &is_null);
                    ((int64_t*)ctx->col_data[c])[row] = is_null ? NULL_I64 : v;
                    if (is_null) my_had_null[c] = true;
                    break;
                }
                case CSV_TYPE_U8: {
                    /* U8 is non-nullable; fast_i64 returns 0 for
                     * empty / unparseable input and we store it as-is. */
                    bool is_null;
                    int64_t v = fast_i64(fld, flen, &is_null);
                    ((uint8_t*)ctx->col_data[c])[row] = (uint8_t)v;
                    break;
                }
                case CSV_TYPE_I16: {
                    bool is_null;
                    int64_t v = fast_i64(fld, flen, &is_null);
                    ((int16_t*)ctx->col_data[c])[row] = is_null ? NULL_I16 : (int16_t)v;
                    if (is_null) my_had_null[c] = true;
                    break;
                }
                case CSV_TYPE_I32: {
                    bool is_null;
                    int64_t v = fast_i64(fld, flen, &is_null);
                    ((int32_t*)ctx->col_data[c])[row] = is_null ? NULL_I32 : (int32_t)v;
                    if (is_null) my_had_null[c] = true;
                    break;
                }
                case CSV_TYPE_F64: {
                    bool is_null;
                    double v = fast_f64(fld, flen, &is_null);
                    ((double*)ctx->col_data[c])[row] = is_null ? NULL_F64 : v;
                    if (is_null) my_had_null[c] = true;
                    break;
                }
                case CSV_TYPE_F32: {
                    bool is_null;
                    double v = fast_f64(fld, flen, &is_null);
                    ((float*)ctx->col_data[c])[row] = is_null ? NULL_F32 : (float)v;
                    if (is_null) my_had_null[c] = true;
                    break;
                }
                case CSV_TYPE_DATE: {
                    bool is_null;
                    int32_t v = fast_date(fld, flen, &is_null);
                    ((int32_t*)ctx->col_data[c])[row] = is_null ? NULL_I32 : v;
                    if (is_null) my_had_null[c] = true;
                    break;
                }
                case CSV_TYPE_TIME: {
                    bool is_null;
                    int32_t v = fast_time(fld, flen, &is_null);
                    ((int32_t*)ctx->col_data[c])[row] = is_null ? NULL_I32 : v;
                    if (is_null) my_had_null[c] = true;
                    break;
                }
                case CSV_TYPE_TIMESTAMP: {
                    bool is_null;
                    int64_t v = fast_timestamp(fld, flen, &is_null);
                    ((int64_t*)ctx->col_data[c])[row] = is_null ? NULL_I64 : v;
                    if (is_null) my_had_null[c] = true;
                    break;
                }
                case CSV_TYPE_GUID: {
                    bool is_null;
                    uint8_t* slot = (uint8_t*)ctx->col_data[c] + (size_t)row * 16;
                    fast_guid(fld, flen, slot, &is_null);
                    if (is_null) {
                        memset(slot, 0, 16);
                        my_had_null[c] = true;
                    }
                    break;
                }
                case CSV_TYPE_STR: {
                    if (flen == 0) {
                        ctx->str_refs[c][row].ptr = NULL;
                        ctx->str_refs[c][row].len = 0;
                        my_had_null[c] = true;
                    } else {
                        /* fld may point into esc_buf (stack) or dyn_esc
                         * (freed below) — both die before csv_fill_str_cols
                         * reads the strref.  Persist escaped fields. */
                        if (fld < ctx->buf || fld >= buf_end) {
                            if (dyn_esc && fld == dyn_esc) {
                                dyn_esc = NULL; /* transfer ownership */
                            } else {
                                char* cp = (char*)ray_sys_alloc(flen);
                                if (cp) { memcpy(cp, fld, flen); fld = cp; }
                            }
                        }
                        ctx->str_refs[c][row].ptr = fld;
                        ctx->str_refs[c][row].len = (uint32_t)flen;
                    }
                    break;
                }
                default:
                    break;
            }
            if (RAY_UNLIKELY(dyn_esc != NULL)) ray_sys_free(dyn_esc);
        }
    }
}

/* --------------------------------------------------------------------------
 * Serial parse fallback (small files or no thread pool)
 * -------------------------------------------------------------------------- */

static void csv_parse_serial(const char* buf, size_t buf_size,
                              const int64_t* row_offsets, int64_t n_rows,
                              int n_cols, char delim,
                              const csv_type_t* col_types,
                              const int8_t* resolved_types,
                              void** col_data,
                              csv_strref_t** str_refs,
                              bool* col_had_null) {
    char esc_buf[8192];
    const char* buf_end = buf + buf_size;

    for (int64_t row = 0; row < n_rows; row++) {
        const char* p = buf + row_offsets[row];
        const char* row_end = (row + 1 < n_rows)
            ? buf + row_offsets[row + 1]
            : buf_end;

        for (int c = 0; c < n_cols; c++) {
            /* Guard: if past row boundary, fill remaining columns with defaults + null */
            if (p >= row_end) {
                for (; c < n_cols; c++) {
                    switch (col_types[c]) {
                        case CSV_TYPE_BOOL: ((uint8_t*)col_data[c])[row] = 0; break;
                        case CSV_TYPE_U8:   ((uint8_t*)col_data[c])[row] = 0; break;
                        case CSV_TYPE_I16:  ((int16_t*)col_data[c])[row] = NULL_I16; break;
                        case CSV_TYPE_I32:  ((int32_t*)col_data[c])[row] = NULL_I32; break;
                        case CSV_TYPE_I64:  ((int64_t*)col_data[c])[row] = NULL_I64; break;
                        case CSV_TYPE_F32:  ((float*)col_data[c])[row] = NULL_F32; break;
                        case CSV_TYPE_F64:  ((double*)col_data[c])[row] = NULL_F64; break;
                        case CSV_TYPE_DATE: ((int32_t*)col_data[c])[row] = NULL_I32; break;
                        case CSV_TYPE_TIME: ((int32_t*)col_data[c])[row] = NULL_I32; break;
                        case CSV_TYPE_TIMESTAMP:
                            ((int64_t*)col_data[c])[row] = NULL_I64; break;
                        case CSV_TYPE_GUID:
                            memset((uint8_t*)col_data[c] + (size_t)row * 16, 0, 16);
                            break;
                        case CSV_TYPE_STR:
                            str_refs[c][row].ptr = NULL;
                            str_refs[c][row].len = 0;
                            break;
                        default: break;
                    }
                    /* BOOL/U8 are non-nullable; empty cells store 0/false. */
                    if (col_types[c] != CSV_TYPE_BOOL &&
                        col_types[c] != CSV_TYPE_U8) {
                        col_had_null[c] = true;
                    }
                }
                break;
            }

            const char* fld;
            size_t flen;
            char* dyn_esc = NULL;
            p = scan_field(p, buf_end, delim, &fld, &flen, esc_buf, &dyn_esc);

            /* Strip trailing \r from last field of row */
            if (c == n_cols - 1 && flen > 0 && fld[flen - 1] == '\r')
                flen--;

            switch (col_types[c]) {
                case CSV_TYPE_BOOL: {
                    /* BOOL is non-nullable. */
                    bool is_null;
                    uint8_t v = fast_bool(fld, flen, &is_null);
                    ((uint8_t*)col_data[c])[row] = v;
                    break;
                }
                case CSV_TYPE_I64: {
                    bool is_null;
                    int64_t v = fast_i64(fld, flen, &is_null);
                    ((int64_t*)col_data[c])[row] = is_null ? NULL_I64 : v;
                    if (is_null) col_had_null[c] = true;
                    break;
                }
                case CSV_TYPE_U8: {
                    /* U8 is non-nullable. */
                    bool is_null;
                    int64_t v = fast_i64(fld, flen, &is_null);
                    ((uint8_t*)col_data[c])[row] = (uint8_t)v;
                    break;
                }
                case CSV_TYPE_I16: {
                    bool is_null;
                    int64_t v = fast_i64(fld, flen, &is_null);
                    ((int16_t*)col_data[c])[row] = is_null ? NULL_I16 : (int16_t)v;
                    if (is_null) col_had_null[c] = true;
                    break;
                }
                case CSV_TYPE_I32: {
                    bool is_null;
                    int64_t v = fast_i64(fld, flen, &is_null);
                    ((int32_t*)col_data[c])[row] = is_null ? NULL_I32 : (int32_t)v;
                    if (is_null) col_had_null[c] = true;
                    break;
                }
                case CSV_TYPE_F64: {
                    bool is_null;
                    double v = fast_f64(fld, flen, &is_null);
                    ((double*)col_data[c])[row] = is_null ? NULL_F64 : v;
                    if (is_null) col_had_null[c] = true;
                    break;
                }
                case CSV_TYPE_F32: {
                    bool is_null;
                    double v = fast_f64(fld, flen, &is_null);
                    ((float*)col_data[c])[row] = is_null ? NULL_F32 : (float)v;
                    if (is_null) col_had_null[c] = true;
                    break;
                }
                case CSV_TYPE_DATE: {
                    bool is_null;
                    int32_t v = fast_date(fld, flen, &is_null);
                    ((int32_t*)col_data[c])[row] = is_null ? NULL_I32 : v;
                    if (is_null) col_had_null[c] = true;
                    break;
                }
                case CSV_TYPE_TIME: {
                    bool is_null;
                    int32_t v = fast_time(fld, flen, &is_null);
                    ((int32_t*)col_data[c])[row] = is_null ? NULL_I32 : v;
                    if (is_null) col_had_null[c] = true;
                    break;
                }
                case CSV_TYPE_TIMESTAMP: {
                    bool is_null;
                    int64_t v = fast_timestamp(fld, flen, &is_null);
                    ((int64_t*)col_data[c])[row] = is_null ? NULL_I64 : v;
                    if (is_null) col_had_null[c] = true;
                    break;
                }
                case CSV_TYPE_GUID: {
                    bool is_null;
                    uint8_t* slot = (uint8_t*)col_data[c] + (size_t)row * 16;
                    fast_guid(fld, flen, slot, &is_null);
                    if (is_null) {
                        memset(slot, 0, 16);
                        col_had_null[c] = true;
                    }
                    break;
                }
                case CSV_TYPE_STR: {
                    if (flen == 0) {
                        str_refs[c][row].ptr = NULL;
                        str_refs[c][row].len = 0;
                        col_had_null[c] = true;
                    } else {
                        /* fld may point into esc_buf (stack) or dyn_esc
                         * (freed below) — both die before csv_fill_str_cols
                         * reads the strref.  Persist escaped fields. */
                        if (fld < buf || fld >= buf_end) {
                            if (dyn_esc && fld == dyn_esc) {
                                dyn_esc = NULL; /* transfer ownership */
                            } else {
                                char* cp = (char*)ray_sys_alloc(flen);
                                if (cp) { memcpy(cp, fld, flen); fld = cp; }
                            }
                        }
                        str_refs[c][row].ptr = fld;
                        str_refs[c][row].len = (uint32_t)flen;
                    }
                    break;
                }
                default:
                    break;
            }
            if (RAY_UNLIKELY(dyn_esc != NULL)) ray_sys_free(dyn_esc);
        }
    }
}

/* Per-column elem size for the hash-attach cap.  Mirrors the integer
 * shapes accepted by ray_index_attach_hash (BOOL/U8/I16/I32/I64/DATE/
 * TIME/TIMESTAMP); returns 0 for floats and dict-backed types so the
 * caller skips them. */
static int csv_hash_elem_size(int8_t t) {
    switch (t) {
    case RAY_BOOL: case RAY_U8:                       return 1;
    case RAY_I16:                                     return 2;
    case RAY_I32: case RAY_DATE:                      return 4;
    case RAY_I64: case RAY_TIME: case RAY_TIMESTAMP:  return 8;
    default:                                          return 0;
    }
}

/* Decide whether `v` is a good candidate for an auto-attached hash
 * index, using only its (already-attached) chunk_zone as the entropy
 * proxy.  A column is "random-shaped" when each chunk's [min, max]
 * covers more than half the global range — i.e. there's effectively
 * no clustering, so the per-chunk zone-skip never excludes a chunk
 * and the only way to accelerate `col == K` is by hashing.
 *
 * The memory cap rejects columns where the hash index (table+chain
 * arrays — ~24 bytes/row at default load factor) would be much larger
 * than the data itself.  We use 5× the column's data bytes as the
 * budget: this comfortably admits I32/I64 numeric IDs (where the
 * index is 3–5× the data) while still excluding narrow types like
 * BOOL/U8/I16 where the index would dwarf the column.
 *
 * Returns 1 to attach, 0 to skip. */
static int csv_should_attach_hash(ray_t* v) {
    if (!v || RAY_IS_ERR(v)) return 0;
    int esz = csv_hash_elem_size(v->type);
    if (esz == 0) return 0;
    /* Need a chunk_zone we can read for entropy estimation. */
    if (!(v->attrs & RAY_ATTR_HAS_INDEX) || !v->index) return 0;
    ray_index_t* ix = ray_index_payload(v->index);
    if (ix->kind != RAY_IDX_CHUNK_ZONE || ix->u.chunk_zone.is_f64) return 0;
    uint32_t n_chunks = ix->u.chunk_zone.n_chunks;
    if (n_chunks < 4) return 0;
    const int64_t* mins = (const int64_t*)ray_data(ix->u.chunk_zone.mins);
    const int64_t* maxs = (const int64_t*)ray_data(ix->u.chunk_zone.maxs);

    /* Whole-column [gmin, gmax] from the chunk extrema, ignoring empty
     * chunks (mn > mx, set by the chunk_zone scan when a chunk is fully
     * null). */
    int64_t gmin = INT64_MAX, gmax = INT64_MIN;
    for (uint32_t g = 0; g < n_chunks; g++) {
        if (mins[g] > maxs[g]) continue;
        if (mins[g] < gmin) gmin = mins[g];
        if (maxs[g] > gmax) gmax = maxs[g];
    }
    if (gmin == INT64_MAX || gmax == INT64_MIN) return 0;
    /* Compute (gmax - gmin) in uint64 space — the signed subtraction
     * overflows when the range spans the full I64 width (e.g. UserID
     * hashing to both sign halves).  Reinterpret as uint64 first;
     * 2's-complement wrap gives the correct |gmax - gmin|. */
    uint64_t global_range = (uint64_t)gmax - (uint64_t)gmin;
    if (global_range == 0) return 0;  /* constant column — pointless */

    /* Average per-chunk span / global range — selectivity proxy.
     * Sum the per-chunk spans as doubles so the accumulation can't
     * overflow when chunks span the full I64 width (uint64 sum
     * across ~150 chunks each ~1.8e19 wide overflows; double has
     * ~15 significant decimal digits, plenty for this coarse ratio).
     *
     * Threshold = 0.5.  mean_ratio measures how much of the global
     * value range an average chunk spans: it is the entropy/clustering
     * signal that decides whether a hash index pays off.  A value
     * near 1.0 means each chunk already covers (almost) the whole
     * range — the column is effectively uniformly random, chunk_zone
     * min/max pruning is useless, so every point lookup degenerates
     * to a full-column scan and a hash index is the only way to make
     * it cheap.  A small ratio means values are clustered, so
     * chunk_zone already prunes most chunks and the index buys little.
     * 0.5 is the natural random-vs-clustered separator: above it a
     * chunk spans more than half the range (no useful zone pruning,
     * index it); below it clustering gives the zone map real pruning
     * power and the index's memory/build cost isn't justified. */
    double dgr = (double)global_range;
    double span_sum = 0.0;
    uint32_t n_eff = 0;
    for (uint32_t g = 0; g < n_chunks; g++) {
        if (mins[g] > maxs[g]) continue;
        uint64_t span = (uint64_t)maxs[g] - (uint64_t)mins[g];
        span_sum += (double)span;
        n_eff++;
    }
    if (n_eff < 4) return 0;
    double mean_ratio = (span_sum / (double)n_eff) / dgr;
    if (mean_ratio <= 0.5) return 0;

    /* Memory cap: ray_index_attach_hash allocates a power-of-two
     * `cap = next_pow2(2*n)` int64 table plus an n-entry int64
     * chain.  Skip when the index would cost more than 5× the
     * column's payload — keeps narrow integer types (where the
     * index dwarfs the data) out of the index set while admitting
     * I32 / I64 numeric IDs.  Done in int64 arithmetic (we cap n
     * to anything that would overflow at the row counts we accept). */
    int64_t n = v->len;
    if (n <= 0) return 0;
    uint64_t cap = 8;
    uint64_t want = (uint64_t)(2 * n);
    while (cap < want) cap <<= 1;
    uint64_t aux_bytes  = cap * 8u + (uint64_t)n * 8u;
    uint64_t data_bytes = (uint64_t)n * (uint64_t)esz;
    if (aux_bytes > 5u * data_bytes) return 0;

    return 1;
}

/* --------------------------------------------------------------------------
 * `INT` schema columns — auto narrowest integer width.
 *
 * The `INT` schema token arrives as RAY_CSV_AUTO_TAG in resolved_types[].
 * Width is a whole-column property (the narrowest type holding every value),
 * so it must be resolved over ALL rows before any column is allocated — the
 * streaming splayed/parted writers materialize the file in 1M-row chunks and
 * the splayed column writer is opened with a fixed width, so a per-chunk
 * narrowing would diverge across chunks/partitions.  We therefore make a
 * single stats scan (min/max/has_null over non-null int64 values), resolve to
 * a concrete width, and rewrite resolved_types[] in place.  Downstream the
 * column then flows through the existing explicit-width parse/write path
 * unchanged — there is no CSV_TYPE_AUTO parse case.
 * -------------------------------------------------------------------------- */

/* Map a resolved narrow-int CSV width back to its RAY column type. */
static int8_t csv_auto_width_to_ray(csv_type_t w) {
    switch (w) {
        case CSV_TYPE_I16:  return RAY_I16;
        case CSV_TYPE_I32:  return RAY_I32;
        default:            return RAY_I64;
    }
}

/* Accumulate per-column min/max/has_null over a block of rows for the columns
 * flagged RAY_CSV_AUTO_TAG in resolved_types; non-AUTO columns are skipped.
 * Mirrors the field-walk (and short-row null handling) of csv_parse_serial. */
static void csv_auto_scan_rows(const char* buf, const char* buf_end,
                               const int64_t* row_offsets, int64_t n_rows,
                               int ncols, char delim,
                               const int8_t* resolved_types,
                               int64_t* col_min, int64_t* col_max,
                               bool* col_had_null) {
    char esc_buf[8192];
    for (int64_t row = 0; row < n_rows; row++) {
        const char* p = buf + row_offsets[row];
        const char* row_end = (row + 1 < n_rows)
            ? buf + row_offsets[row + 1] : buf_end;
        for (int c = 0; c < ncols; c++) {
            if (p >= row_end) {
                /* Short row — remaining AUTO cells are empty (null). */
                for (; c < ncols; c++)
                    if (resolved_types[c] == RAY_CSV_AUTO_TAG)
                        col_had_null[c] = true;
                break;
            }
            const char* fld;
            size_t flen;
            char* dyn_esc = NULL;
            p = scan_field(p, buf_end, delim, &fld, &flen, esc_buf, &dyn_esc);
            if (c == ncols - 1 && flen > 0 && fld[flen - 1] == '\r') flen--;
            if (resolved_types[c] == RAY_CSV_AUTO_TAG) {
                bool is_null;
                int64_t v = fast_i64(fld, flen, &is_null);
                if (is_null) {
                    col_had_null[c] = true;
                } else {
                    if (v < col_min[c]) col_min[c] = v;
                    if (v > col_max[c]) col_max[c] = v;
                }
            }
            if (RAY_UNLIKELY(dyn_esc != NULL)) ray_sys_free(dyn_esc);
        }
    }
}

/* Resolve every RAY_CSV_AUTO_TAG column in resolved_types[] to a concrete
 * width using a stats scan over the already-built whole-file row_offsets.
 * No-op when the schema carries no `INT` column. */
static void csv_resolve_auto_in_place(const char* buf, size_t file_size,
                                      const int64_t* row_offsets, int64_t n_rows,
                                      int ncols, char delim,
                                      int8_t* resolved_types) {
    bool any = false;
    for (int c = 0; c < ncols; c++)
        if (resolved_types[c] == RAY_CSV_AUTO_TAG) any = true;
    if (!any) return;

    int64_t col_min[CSV_MAX_COLS], col_max[CSV_MAX_COLS];
    bool col_had_null[CSV_MAX_COLS];
    for (int c = 0; c < ncols; c++) {
        col_min[c] = INT64_MAX; col_max[c] = INT64_MIN; col_had_null[c] = false;
    }
    csv_auto_scan_rows(buf, buf + file_size, row_offsets, n_rows, ncols, delim,
                       resolved_types, col_min, col_max, col_had_null);
    for (int c = 0; c < ncols; c++)
        if (resolved_types[c] == RAY_CSV_AUTO_TAG)
            resolved_types[c] = csv_auto_width_to_ray(
                csv_resolve_int_width(col_min[c], col_max[c], col_had_null[c]));
}

/* Same as csv_resolve_auto_in_place, but for the streaming writers which never
 * build whole-file offsets: stream the file in chunks accumulating stats,
 * then resolve.  Bounded memory (one chunk of offsets at a time). */
static void csv_resolve_auto_streamed(const char* buf, size_t file_size,
                                      size_t data_offset, int ncols, char delim,
                                      bool data_has_quotes,
                                      int8_t* resolved_types) {
    bool any = false;
    for (int c = 0; c < ncols; c++)
        if (resolved_types[c] == RAY_CSV_AUTO_TAG) any = true;
    if (!any) return;

    int64_t col_min[CSV_MAX_COLS], col_max[CSV_MAX_COLS];
    bool col_had_null[CSV_MAX_COLS];
    for (int c = 0; c < ncols; c++) {
        col_min[c] = INT64_MAX; col_max[c] = INT64_MIN; col_had_null[c] = false;
    }

    size_t off = data_offset;
    while (off < file_size) {
        ray_t* hdr = NULL;
        int64_t* roff = NULL;
        size_t next = off;
        int64_t cnt = build_row_offsets_limited(buf, file_size, off,
                                                CSV_PART_ROWS_DEFAULT,
                                                data_has_quotes,
                                                &roff, &hdr, &next);
        if (cnt <= 0) { scratch_free(hdr); break; }
        csv_auto_scan_rows(buf, buf + file_size, roff, cnt, ncols, delim,
                           resolved_types, col_min, col_max, col_had_null);
        scratch_free(hdr);
        if (next <= off) break;
        off = next;
    }

    for (int c = 0; c < ncols; c++)
        if (resolved_types[c] == RAY_CSV_AUTO_TAG)
            resolved_types[c] = csv_auto_width_to_ray(
                csv_resolve_int_width(col_min[c], col_max[c], col_had_null[c]));
}

static ray_t* csv_materialize_rows(const char* buf, size_t file_size,
                                   const int64_t* row_offsets, int64_t n_rows,
                                   int ncols, char delimiter,
                                   const int64_t* col_name_ids,
                                   const int8_t* resolved_types) {
    /* Defensive guard: RAY_CSV_AUTO_TAG must be resolved to a concrete width
     * before reaching this point (by csv_resolve_auto_in_place or
     * csv_resolve_auto_streamed).  If a marker slips through, the resolution
     * step was bypassed — loud failure prevents a silent ray_vec_new(120,…). */
    for (int c = 0; c < ncols; c++) {
        if (resolved_types[c] == RAY_CSV_AUTO_TAG) {
            fprintf(stderr,
                "csv: BUG: unresolved INT (RAY_CSV_AUTO_TAG=%d) reached "
                "csv_materialize_rows for column %d\n",
                (int)RAY_CSV_AUTO_TAG, c);
            return NULL;
        }
    }

    ray_t* col_vecs[CSV_MAX_COLS];
    void* col_data[CSV_MAX_COLS];

    for (int c = 0; c < ncols; c++) {
        int8_t type = resolved_types[c];
        col_vecs[c] = (type == RAY_SYM) ? ray_sym_vec_new(RAY_SYM_W32, n_rows)
                                        : ray_vec_new(type, n_rows);
        if (!col_vecs[c] || RAY_IS_ERR(col_vecs[c])) {
            for (int j = 0; j < c; j++) ray_release(col_vecs[j]);
            return NULL;
        }
        col_vecs[c]->len = n_rows;
        col_data[c] = ray_data(col_vecs[c]);
    }

    bool col_had_null[CSV_MAX_COLS];
    if (ncols > 0) memset(col_had_null, 0, (size_t)ncols * sizeof(bool));

    csv_type_t parse_types[CSV_MAX_COLS];
    for (int c = 0; c < ncols; c++) {
        switch (resolved_types[c]) {
            case RAY_BOOL:      parse_types[c] = CSV_TYPE_BOOL;      break;
            case RAY_U8:        parse_types[c] = CSV_TYPE_U8;        break;
            case RAY_I16:       parse_types[c] = CSV_TYPE_I16;       break;
            case RAY_I32:       parse_types[c] = CSV_TYPE_I32;       break;
            case RAY_I64:       parse_types[c] = CSV_TYPE_I64;       break;
            case RAY_F32:       parse_types[c] = CSV_TYPE_F32;       break;
            case RAY_F64:       parse_types[c] = CSV_TYPE_F64;       break;
            case RAY_DATE:      parse_types[c] = CSV_TYPE_DATE;      break;
            case RAY_TIME:      parse_types[c] = CSV_TYPE_TIME;      break;
            case RAY_TIMESTAMP: parse_types[c] = CSV_TYPE_TIMESTAMP; break;
            case RAY_GUID:      parse_types[c] = CSV_TYPE_GUID;      break;
            default:            parse_types[c] = CSV_TYPE_STR;       break;
        }
    }

    int64_t sym_max_ids[CSV_MAX_COLS];
    memset(sym_max_ids, 0, (size_t)ncols * sizeof(int64_t));

    int has_text_cols = 0;
    for (int c = 0; c < ncols; c++) {
        if (parse_types[c] == CSV_TYPE_STR) {
            has_text_cols = 1;
            break;
        }
    }

    csv_strref_t* str_ref_bufs[CSV_MAX_COLS];
    ray_t* str_ref_hdrs[CSV_MAX_COLS];
    memset(str_ref_bufs, 0, sizeof(str_ref_bufs));
    memset(str_ref_hdrs, 0, sizeof(str_ref_hdrs));
    for (int c = 0; c < ncols; c++) {
        if (parse_types[c] == CSV_TYPE_STR) {
            size_t sz = (size_t)n_rows * sizeof(csv_strref_t);
            str_ref_bufs[c] = (csv_strref_t*)scratch_alloc(&str_ref_hdrs[c], sz);
            if (!str_ref_bufs[c]) {
                for (int j = 0; j < ncols; j++) ray_release(col_vecs[j]);
                for (int j = 0; j < c; j++) scratch_free(str_ref_hdrs[j]);
                return NULL;
            }
        }
    }

    {
        ray_pool_t* pool = ray_pool_get();
        bool use_parallel = pool && n_rows > 8192;

        if (use_parallel) {
            uint32_t n_workers = ray_pool_total_workers(pool);
            size_t whn_sz = (size_t)n_workers * (size_t)ncols * sizeof(bool);
            bool* worker_had_null_buf = (bool*)ray_sys_alloc(whn_sz);
            if (!worker_had_null_buf) {
                use_parallel = false;
            } else {
                memset(worker_had_null_buf, 0, whn_sz);

                csv_par_ctx_t ctx = {
                    .buf              = buf,
                    .buf_size         = file_size,
                    .row_offsets      = row_offsets,
                    .n_rows           = n_rows,
                    .n_cols           = ncols,
                    .delim            = delimiter,
                    .col_types        = parse_types,
                    .resolved_types   = resolved_types,
                    .col_data         = col_data,
                    .str_refs         = str_ref_bufs,
                    .worker_had_null  = worker_had_null_buf,
                };

                ray_pool_dispatch(pool, csv_parse_fn, &ctx, n_rows);

                for (uint32_t w = 0; w < n_workers; w++) {
                    for (int c = 0; c < ncols; c++) {
                        if (worker_had_null_buf[(size_t)w * (size_t)ncols + (size_t)c])
                            col_had_null[c] = true;
                    }
                }
                ray_sys_free(worker_had_null_buf);
            }
        }

        if (!use_parallel) {
            csv_parse_serial(buf, file_size, row_offsets, n_rows,
                             ncols, delimiter, parse_types, resolved_types, col_data,
                             str_ref_bufs, col_had_null);
        }
    }

    if (has_text_cols) {
        csv_finalize_ctx_t fctx = {
            .str_refs       = str_ref_bufs,
            .n_cols         = ncols,
            .parse_types    = parse_types,
            .resolved_types = resolved_types,
            .col_data       = col_data,
            .col_vecs       = col_vecs,
            .n_rows         = n_rows,
            .sym_max_ids    = sym_max_ids,
            .fill_ok        = true,
            .intern_ok      = true,
        };
        ray_pool_t* fpool = ray_pool_get();
        if (fpool && ray_pool_total_workers(fpool) >= 2) {
            ray_pool_dispatch_n(fpool, csv_finalize_task, &fctx, 2);
        } else {
            csv_finalize_task(&fctx, 0, 0, 1);
            csv_finalize_task(&fctx, 0, 1, 2);
        }
        if (!fctx.fill_ok || !fctx.intern_ok) {
            csv_free_escaped_strrefs(str_ref_bufs, ncols, parse_types, n_rows, buf, file_size);
            for (int c = 0; c < ncols; c++) scratch_free(str_ref_hdrs[c]);
            for (int c = 0; c < ncols; c++) ray_release(col_vecs[c]);
            return NULL;
        }
    }

    for (int c = 0; c < ncols; c++) {
        if (resolved_types[c] != RAY_SYM) continue;
        uint32_t* ids = (uint32_t*)col_data[c];
        int64_t max_id = 0;
        for (int64_t r = 0; r < n_rows; r++)
            if ((int64_t)ids[r] > max_id) max_id = ids[r];
        sym_max_ids[c] = max_id;
    }

    csv_free_escaped_strrefs(str_ref_bufs, ncols, parse_types, n_rows, buf, file_size);
    for (int c = 0; c < ncols; c++) scratch_free(str_ref_hdrs[c]);

    for (int c = 0; c < ncols; c++) {
        ray_t* vec = col_vecs[c];
        /* SYM and STR both represent a blank cell as the empty string ("" /
         * sym 0) — there is no null distinct from empty, so neither type
         * carries HAS_NULLS (a per-row null check on them is pure waste). */
        if (col_had_null[c] && vec->type != RAY_SYM && vec->type != RAY_STR)
            vec->attrs |= RAY_ATTR_HAS_NULLS;
        else
            vec->attrs &= (uint8_t)~RAY_ATTR_HAS_NULLS;
    }

    for (int c = 0; c < ncols; c++) {
        if (resolved_types[c] != RAY_SYM) continue;
        uint8_t new_w = ray_sym_dict_width(sym_max_ids[c]);
        if (new_w >= RAY_SYM_W32) continue;
        ray_t* narrow = ray_sym_vec_new(new_w, n_rows);
        if (!narrow || RAY_IS_ERR(narrow)) continue;
        narrow->len = n_rows;
        const uint32_t* src = (const uint32_t*)col_data[c];
        void* dst = ray_data(narrow);
        if (new_w == RAY_SYM_W8) {
            uint8_t* d = (uint8_t*)dst;
            for (int64_t r = 0; r < n_rows; r++) d[r] = (uint8_t)src[r];
        } else {
            uint16_t* d = (uint16_t*)dst;
            for (int64_t r = 0; r < n_rows; r++) d[r] = (uint16_t)src[r];
        }
        narrow->attrs |= (col_vecs[c]->attrs & RAY_ATTR_HAS_NULLS);
        ray_release(col_vecs[c]);
        col_vecs[c] = narrow;
        col_data[c] = dst;
    }

    /* Per-chunk min/max + null bit on every column big enough to be worth
     * indexing — gives the reduce min/max and the filter chunk-skip paths
     * an O(n_chunks) scan instead of O(n_rows).  Attach is best-effort:
     * unsupported types (RAY_STR/RAY_SYM/RAY_GUID in v1) just stay
     * unindexed and the consumer falls back to a row scan.
     *
     * After the chunk_zone attaches we re-walk the same columns and
     * upgrade the high-entropy ones to a hash index (the chunk_zone
     * stays as well — it's the entropy signal we just measured).  See
     * csv_should_attach_hash for the selectivity + memory cap. */
    for (int c = 0; c < ncols; c++) {
        ray_t* v = col_vecs[c];
        if (!v || RAY_IS_ERR(v)) continue;
        if (v->len < (1 << 16)) continue;        /* < one chunk, skip */
        ray_t* r = ray_index_attach_chunk_zone(&v, 16);
        if (r && !RAY_IS_ERR(r)) col_vecs[c] = v;  /* attach succeeded */
        /* On failure the original column stays in col_vecs[c]; ignore. */
    }
    for (int c = 0; c < ncols; c++) {
        ray_t* v = col_vecs[c];
        if (!csv_should_attach_hash(v)) continue;
        /* ray_index_attach_hash drops any existing index on the
         * column first; the chunk_zone we just built is sacrificed
         * for the hash.  That's the right trade — once the column
         * is known to be high-entropy, chunk-skip never fires
         * anyway, so the chunk_zone is dead weight. */
        ray_t* r = ray_index_attach_hash(&v);
        if (r && !RAY_IS_ERR(r)) col_vecs[c] = v;
    }

    ray_t* tbl = ray_table_new(ncols);
    if (!tbl || RAY_IS_ERR(tbl)) {
        for (int c = 0; c < ncols; c++) ray_release(col_vecs[c]);
        return NULL;
    }

    for (int c = 0; c < ncols; c++) {
        tbl = ray_table_add_col(tbl, col_name_ids[c], col_vecs[c]);
        ray_release(col_vecs[c]);
    }

    return tbl;
}

/* When explicit column names are supplied the caller declares "no header", but
 * the file may still carry a matching header row.  Parsing that header row as
 * data is a silent +1-row corruption.  Guard against it: if
 * the first line's fields all equal the supplied names, it IS a header — return
 * the offset of the data that follows it.  Returns NULL when the first line is
 * genuine data (no field-vs-name mismatch is possible for a real data row that
 * happens to equal every column name, so this never eats a data row). */
static const char* csv_skip_matching_header(const char* p, const char* buf_end,
                                            char delimiter, int ncols,
                                            const int64_t* names, char* esc_buf) {
    bool is_header = true;
    for (int c = 0; c < ncols; c++) {
        const char* fld; size_t flen; char* dyn = NULL;
        p = scan_field(p, buf_end, delimiter, &fld, &flen, esc_buf, &dyn);
        if (is_header) {
            ray_t* nm = ray_sym_str(names[c]);
            const char* ns = nm ? ray_str_ptr(nm) : NULL;
            size_t nl = nm ? ray_str_len(nm) : 0;
            if (!ns || flen != nl || memcmp(fld, ns, flen) != 0) is_header = false;
        }
        if (dyn) ray_sys_free(dyn);
    }
    if (!is_header) return NULL;
    if (p < buf_end && *p == '\r') p++;
    if (p < buf_end && *p == '\n') p++;
    return p;
}

/* --------------------------------------------------------------------------
 * ray_read_csv_opts — main CSV parser
 * -------------------------------------------------------------------------- */

ray_t* ray_read_csv_named_opts(const char* path, char delimiter, bool header,
                               const int8_t* col_types_in, int32_t n_types,
                               const int64_t* col_names_in, int32_t n_names) {
    /* ---- 1. Open file and get size ---- */
    int fd = open(path, O_RDONLY);
    if (fd < 0) return ray_error("io", NULL);

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return ray_error("io", NULL);
    }
    size_t file_size = (size_t)st.st_size;

    /* ---- 2. mmap the file ---- */
    char* buf = (char*)ray_vm_map_fd_ro(fd, file_size);
    close(fd);
    if (!buf) return ray_error("io", NULL);

#ifdef __APPLE__
    madvise(buf, file_size, MADV_SEQUENTIAL);
#endif

    const char* buf_end = buf + file_size;
    ray_t* result = NULL;

    /* ---- 3. Detect delimiter ---- */
    /* Delimiter auto-detected from header row only. Files where the header
     * has a different delimiter distribution than data rows may be misdetected;
     * pass an explicit delimiter for such files.  Scanning additional data rows
     * was considered but adds complexity for a rare edge case. */
    if (delimiter == 0) {
        int commas = 0, tabs = 0;
        for (const char* p = buf; p < buf_end && *p != '\n'; p++) {
            if (*p == ',') commas++;
            if (*p == '\t') tabs++;
        }
        delimiter = (tabs > commas) ? '\t' : ',';
    }

    /* ---- 4. Count columns from first line ---- */
    int ncols = 1;
    {
        const char* p = buf;
        bool in_quote = false;
        while (p < buf_end && (in_quote || (*p != '\n' && *p != '\r'))) {
            if (*p == '"') in_quote = !in_quote;
            else if (!in_quote && *p == delimiter) ncols++;
            p++;
        }
    }
    if (ncols > CSV_MAX_COLS) {
        ray_vm_unmap_file(buf, file_size);
        /* fd already closed after mmap (line 1044) — do not close again */
        /* too many columns */
        return ray_error("range", "csv read: header has too many columns, got %lld (max %d)",
                         (long long)ncols, CSV_MAX_COLS);
    }

    /* ---- 5. Parse header row ---- */
    const char* p = buf;
    char esc_buf[8192];
    int64_t col_name_ids[CSV_MAX_COLS];

    if (header) {
        for (int c = 0; c < ncols; c++) {
            const char* fld;
            size_t flen;
            char* dyn_esc = NULL;
            p = scan_field(p, buf_end, delimiter, &fld, &flen, esc_buf, &dyn_esc);
            col_name_ids[c] = ray_sym_intern(fld, flen);
            if (dyn_esc) ray_sys_free(dyn_esc);
        }
        /* Consume exactly one line terminator (\r, \n, or \r\n) after the
         * header row — NOT a run of newlines, because subsequent empty
         * lines are null data rows. */
        if (p < buf_end && *p == '\r') p++;
        if (p < buf_end && *p == '\n') p++;
    } else if (col_names_in && n_names >= ncols) {
        for (int c = 0; c < ncols; c++)
            col_name_ids[c] = col_names_in[c];
        const char* after = csv_skip_matching_header(buf, buf_end, delimiter,
                                                     ncols, col_names_in, esc_buf);
        if (after) p = after;   /* file carried a header matching the names */
    } else {
        for (int c = 0; c < ncols; c++) {
            char name[32];
            snprintf(name, sizeof(name), "V%d", c + 1);
            col_name_ids[c] = ray_sym_intern(name, strlen(name));
        }
    }

    size_t data_offset = (size_t)(p - buf);

    /* ---- 6. Build row offsets (memchr-accelerated) ---- */
    ray_t* row_offsets_hdr = NULL;
    int64_t* row_offsets = NULL;
    int64_t n_rows = build_row_offsets(buf, file_size, data_offset,
                                        &row_offsets, &row_offsets_hdr);

    if (n_rows == 0) {
        /* Empty file → empty table */
        ray_t* tbl = ray_table_new(ncols);
        if (!tbl || RAY_IS_ERR(tbl)) goto fail_unmap;
        for (int c = 0; c < ncols; c++) {
            ray_t* empty_vec = ray_vec_new(RAY_F64, 0);
            if (empty_vec && !RAY_IS_ERR(empty_vec)) {
                tbl = ray_table_add_col(tbl, col_name_ids[c], empty_vec);
                ray_release(empty_vec);
            }
        }
        ray_vm_unmap_file(buf, file_size);
        return tbl;
    }

    /* ---- 7. Resolve column types ---- */
    int8_t resolved_types[CSV_MAX_COLS];
    if (col_types_in && n_types >= ncols) {
        /* Explicit types provided by caller — validate against known types */
        for (int c = 0; c < ncols; c++) {
            int8_t t = col_types_in[c];
            if (t < RAY_BOOL ||
                (t >= RAY_TYPE_COUNT && t != RAY_CSV_AUTO_TAG) ||
                t == RAY_TABLE) {
                /* Invalid type constant — fall through to error */
                goto fail_offsets;
            }
            resolved_types[c] = t;
        }
    } else if (!col_types_in) {
        if (!csv_infer_types_from_offsets(buf, buf_end, row_offsets, n_rows,
                                          ncols, delimiter, esc_buf,
                                          resolved_types))
            goto fail_offsets;
    } else {
        /* col_types_in provided but too short — error */
        goto fail_offsets;
    }

    /* `INT` schema columns: resolve to a concrete narrowest width over all
     * rows before allocating vectors, so the explicit-width path below sees a
     * real type. */
    csv_resolve_auto_in_place(buf, file_size, row_offsets, n_rows,
                              ncols, delimiter, resolved_types);

    /* ---- 8. Allocate column vectors ---- */
    ray_t* col_vecs[CSV_MAX_COLS];
    void* col_data[CSV_MAX_COLS];

    for (int c = 0; c < ncols; c++) {
        int8_t type = resolved_types[c];
        /* String columns: allocate RAY_SYM at W32 (4B/elem) for sym IDs.
         * After intern, narrow to W8/W16 if max sym ID permits. */
        col_vecs[c] = (type == RAY_SYM) ? ray_sym_vec_new(RAY_SYM_W32, n_rows)
                                        : ray_vec_new(type, n_rows);
        if (!col_vecs[c] || RAY_IS_ERR(col_vecs[c])) {
            for (int j = 0; j < c; j++) ray_release(col_vecs[j]);
            goto fail_offsets;
        }
        /* len set early so parallel workers can write to full extent;
         * parse errors return before table is used. */
        col_vecs[c]->len = n_rows;
        col_data[c] = ray_data(col_vecs[c]);
    }

    bool col_had_null[CSV_MAX_COLS];
    if (ncols > 0) memset(col_had_null, 0, (size_t)ncols * sizeof(bool));

    /* Build csv_type_t array for parse functions (maps td types → csv types) */
    csv_type_t parse_types[CSV_MAX_COLS];
    for (int c = 0; c < ncols; c++) {
        switch (resolved_types[c]) {
            case RAY_BOOL:      parse_types[c] = CSV_TYPE_BOOL;      break;
            case RAY_U8:        parse_types[c] = CSV_TYPE_U8;        break;
            case RAY_I16:       parse_types[c] = CSV_TYPE_I16;       break;
            case RAY_I32:       parse_types[c] = CSV_TYPE_I32;       break;
            case RAY_I64:       parse_types[c] = CSV_TYPE_I64;       break;
            case RAY_F32:       parse_types[c] = CSV_TYPE_F32;       break;
            case RAY_F64:       parse_types[c] = CSV_TYPE_F64;       break;
            case RAY_DATE:      parse_types[c] = CSV_TYPE_DATE;      break;
            case RAY_TIME:      parse_types[c] = CSV_TYPE_TIME;      break;
            case RAY_TIMESTAMP: parse_types[c] = CSV_TYPE_TIMESTAMP; break;
            case RAY_GUID:      parse_types[c] = CSV_TYPE_GUID;      break;
            default:           parse_types[c] = CSV_TYPE_STR;       break;
        }
    }

    /* ---- 9. Parse data ---- */
    int64_t sym_max_ids[CSV_MAX_COLS];
    memset(sym_max_ids, 0, (size_t)ncols * sizeof(int64_t));

    /* Check if any materialized string columns exist */
    int has_text_cols = 0;
    for (int c = 0; c < ncols; c++) {
        if (parse_types[c] == CSV_TYPE_STR) {
            has_text_cols = 1;
            break;
        }
    }

    csv_strref_t* str_ref_bufs[CSV_MAX_COLS];
    ray_t* str_ref_hdrs[CSV_MAX_COLS];
    memset(str_ref_bufs, 0, sizeof(str_ref_bufs));
    memset(str_ref_hdrs, 0, sizeof(str_ref_hdrs));
    for (int c = 0; c < ncols; c++) {
        if (parse_types[c] == CSV_TYPE_STR) {
            size_t sz = (size_t)n_rows * sizeof(csv_strref_t);
            str_ref_bufs[c] = (csv_strref_t*)scratch_alloc(&str_ref_hdrs[c], sz);
            if (!str_ref_bufs[c]) {
                for (int j = 0; j < ncols; j++) ray_release(col_vecs[j]);
                for (int j = 0; j < c; j++) scratch_free(str_ref_hdrs[j]);
                goto fail_offsets;
            }
        }
    }

    {
        ray_pool_t* pool = ray_pool_get();
        bool use_parallel = pool && n_rows > 8192;

        if (use_parallel) {
            uint32_t n_workers = ray_pool_total_workers(pool);
            size_t whn_sz = (size_t)n_workers * (size_t)ncols * sizeof(bool);
            bool* worker_had_null_buf = (bool*)ray_sys_alloc(whn_sz);
            if (!worker_had_null_buf) {
                use_parallel = false;
            } else {
                memset(worker_had_null_buf, 0, whn_sz);

                csv_par_ctx_t ctx = {
                    .buf              = buf,
                    .buf_size         = file_size,
                    .row_offsets      = row_offsets,
                    .n_rows           = n_rows,
                    .n_cols           = ncols,
                    .delim            = delimiter,
                    .col_types        = parse_types,
                    .resolved_types   = resolved_types,
                    .col_data         = col_data,
                    .str_refs         = str_ref_bufs,
                    .worker_had_null  = worker_had_null_buf,
                };

                ray_pool_dispatch(pool, csv_parse_fn, &ctx, n_rows);

                /* OR worker null flags into col_had_null */
                for (uint32_t w = 0; w < n_workers; w++) {
                    for (int c = 0; c < ncols; c++) {
                        if (worker_had_null_buf[(size_t)w * (size_t)ncols + (size_t)c])
                            col_had_null[c] = true;
                    }
                }
                ray_sys_free(worker_had_null_buf);
            }
        }

        if (!use_parallel) {
            csv_parse_serial(buf, file_size, row_offsets, n_rows,
                             ncols, delimiter, parse_types, resolved_types, col_data,
                             str_ref_bufs, col_had_null);
        }
    }

    /* ---- 9b. Materialize RAY_STR columns AND batch-intern sym columns ----
     * These two phases touch disjoint columns and (after the GUID fix)
     * intern_strings is the only one that mutates the global sym table.
     * Dispatch them as two thread-pool tasks so they overlap in wall time
     * — typically saves the smaller of the two phases. */
    if (has_text_cols) {
        csv_finalize_ctx_t fctx = {
            .str_refs       = str_ref_bufs,
            .n_cols         = ncols,
            .parse_types    = parse_types,
            .resolved_types = resolved_types,
            .col_data       = col_data,
            .col_vecs       = col_vecs,
            .n_rows         = n_rows,
            .sym_max_ids    = sym_max_ids,
            .fill_ok        = true,
            .intern_ok      = true,
        };
        ray_pool_t* fpool = ray_pool_get();
        if (fpool && ray_pool_total_workers(fpool) >= 2) {
            ray_pool_dispatch_n(fpool, csv_finalize_task, &fctx, 2);
        } else {
            csv_finalize_task(&fctx, 0, 0, 1);
            csv_finalize_task(&fctx, 0, 1, 2);
        }
        if (!fctx.fill_ok || !fctx.intern_ok) {
            csv_free_escaped_strrefs(str_ref_bufs, ncols, parse_types, n_rows, buf, file_size);
            for (int c = 0; c < ncols; c++) scratch_free(str_ref_hdrs[c]);
            for (int c = 0; c < ncols; c++) ray_release(col_vecs[c]);
            goto fail_offsets;
        }
    }

    for (int c = 0; c < ncols; c++) {
        if (resolved_types[c] != RAY_SYM) continue;
        uint32_t* ids = (uint32_t*)col_data[c];
        int64_t max_id = 0;
        for (int64_t r = 0; r < n_rows; r++)
            if ((int64_t)ids[r] > max_id) max_id = ids[r];
        sym_max_ids[c] = max_id;
    }

    /* Free heap-allocated escaped string copies, then strref buffers */
    csv_free_escaped_strrefs(str_ref_bufs, ncols, parse_types, n_rows, buf, file_size);
    for (int c = 0; c < ncols; c++) scratch_free(str_ref_hdrs[c]);

    /* ---- 9c. Set HAS_NULLS for columns that saw a null ----
     *
     * Sentinels in the payload carry the null state; HAS_NULLS is the
     * vec-level fast-path bit.  SYM columns are no-null by design —
     * empty fields were already remapped to sym 0 in step 9b. */
    for (int c = 0; c < ncols; c++) {
        ray_t* vec = col_vecs[c];
        /* SYM and STR both represent a blank cell as the empty string ("" /
         * sym 0) — there is no null distinct from empty, so neither type
         * carries HAS_NULLS (a per-row null check on them is pure waste). */
        if (col_had_null[c] && vec->type != RAY_SYM && vec->type != RAY_STR)
            vec->attrs |= RAY_ATTR_HAS_NULLS;
        else
            vec->attrs &= (uint8_t)~RAY_ATTR_HAS_NULLS;
    }

    /* ---- 10. Narrow sym columns to optimal width ---- */
    for (int c = 0; c < ncols; c++) {
        if (resolved_types[c] != RAY_SYM) continue;
        uint8_t new_w = ray_sym_dict_width(sym_max_ids[c]);
        if (new_w >= RAY_SYM_W32) continue; /* already at W32, no savings */
        ray_t* narrow = ray_sym_vec_new(new_w, n_rows);
        if (!narrow || RAY_IS_ERR(narrow)) continue;
        narrow->len = n_rows;
        const uint32_t* src = (const uint32_t*)col_data[c];
        void* dst = ray_data(narrow);
        if (new_w == RAY_SYM_W8) {
            uint8_t* d = (uint8_t*)dst;
            for (int64_t r = 0; r < n_rows; r++) d[r] = (uint8_t)src[r];
        } else { /* RAY_SYM_W16 */
            uint16_t* d = (uint16_t*)dst;
            for (int64_t r = 0; r < n_rows; r++) d[r] = (uint16_t)src[r];
        }
        narrow->attrs |= (col_vecs[c]->attrs & RAY_ATTR_HAS_NULLS);
        ray_release(col_vecs[c]);
        col_vecs[c] = narrow;
        col_data[c] = dst;
    }

    /* ---- 11. Build table ---- */
    {
        /* Best-effort per-chunk zone index attach (see comment on the
         * matching loop in build_table_from_cols) — unsupported types
         * fall through to the unindexed path inside the consumer.
         * Second pass upgrades high-entropy columns to a hash index;
         * see csv_should_attach_hash. */
        for (int c = 0; c < ncols; c++) {
            ray_t* v = col_vecs[c];
            if (!v || RAY_IS_ERR(v)) continue;
            if (v->len < (1 << 16)) continue;
            ray_t* r = ray_index_attach_chunk_zone(&v, 16);
            if (r && !RAY_IS_ERR(r)) col_vecs[c] = v;
        }
        for (int c = 0; c < ncols; c++) {
            ray_t* v = col_vecs[c];
            if (!csv_should_attach_hash(v)) continue;
            ray_t* r = ray_index_attach_hash(&v);
            if (r && !RAY_IS_ERR(r)) col_vecs[c] = v;
        }

        ray_t* tbl = ray_table_new(ncols);
        if (!tbl || RAY_IS_ERR(tbl)) {
            for (int c = 0; c < ncols; c++) ray_release(col_vecs[c]);
            goto fail_offsets;
        }

        for (int c = 0; c < ncols; c++) {
            tbl = ray_table_add_col(tbl, col_name_ids[c], col_vecs[c]);
            ray_release(col_vecs[c]);
        }

        result = tbl;
    }

    /* ---- 12. Cleanup ---- */
    scratch_free(row_offsets_hdr);
    ray_vm_unmap_file(buf, file_size);
    return result;

    /* Error paths */
fail_offsets:
    scratch_free(row_offsets_hdr);
fail_unmap:
    ray_vm_unmap_file(buf, file_size);
    return ray_error("oom", NULL);
}

ray_t* ray_read_csv_opts(const char* path, char delimiter, bool header,
                        const int8_t* col_types_in, int32_t n_types) {
    return ray_read_csv_named_opts(path, delimiter, header,
                                  col_types_in, n_types, NULL, 0);
}

typedef struct {
    FILE* fp;
    char path[1024];
    char tmp_path[1024];
    int8_t type;
    uint8_t attrs;
    int64_t rows;
    bool had_nulls;
    /* SYM columns: the target symfile's domain (shared, borrowed from
     * the caller).  Cells are encoded as positions in it — the freshly
     * parsed chunk vecs are runtime-domain, so each cell's string is
     * find-or-appended into the domain (the flip, Task 7b).  Width is
     * fixed at W32: a streaming writer can't know the final vocabulary
     * before the last chunk, and W32 covers any STRL count. */
    struct ray_sym_domain_s* dom;
} csv_splayed_col_writer_t;

static ray_err_t csv_splayed_writer_open(csv_splayed_col_writer_t* w,
                                         const char* dir, int64_t name_id,
                                         int8_t type,
                                         struct ray_sym_domain_s* dom) {
    memset(w, 0, sizeof(*w));
    w->type = type;
    w->attrs = (type == RAY_SYM) ? RAY_SYM_W32 : 0;
    w->dom = dom;
    if (type == RAY_SYM && !dom) return RAY_ERR_IO;

    ray_t* name_atom = ray_sym_str(name_id);
    if (!name_atom) return RAY_ERR_CORRUPT;
    const char* name = ray_str_ptr(name_atom);
    size_t name_len = ray_str_len(name_atom);
    if (name_len == 0 || name[0] == '.' ||
        memchr(name, '/', name_len) || memchr(name, '\\', name_len) ||
        memchr(name, '\0', name_len))
        return RAY_ERR_DOMAIN;

    int n = snprintf(w->path, sizeof(w->path), "%s/%.*s",
                     dir, (int)name_len, name);
    if (n < 0 || (size_t)n >= sizeof(w->path)) return RAY_ERR_RANGE;
    n = snprintf(w->tmp_path, sizeof(w->tmp_path), "%s.tmp", w->path);
    if (n < 0 || (size_t)n >= sizeof(w->tmp_path)) return RAY_ERR_RANGE;

    w->fp = fopen(w->tmp_path, "wb+");
    if (!w->fp) return RAY_ERR_IO;
    ray_t zero = {0};
    if (fwrite(&zero, 1, 32, w->fp) != 32) return RAY_ERR_IO;
    return RAY_OK;
}

static ray_err_t csv_splayed_writer_append(csv_splayed_col_writer_t* w,
                                           ray_t* col) {
    if (!w->fp || !col || RAY_IS_ERR(col)) return RAY_ERR_TYPE;
    int64_t n = col->len;
    if (n < 0) return RAY_ERR_CORRUPT;

    if (w->type == RAY_SYM) {
        /* Encode cells as positions in the target symfile's domain:
         * resolve each cell through the chunk vec's own domain and
         * find-or-append into the target (distinct work rides the
         * write).  The domain is flushed before the column files are
         * committed (close), preserving the sym-first crash ordering. */
        uint32_t buf[8192];
        for (int64_t off = 0; off < n; ) {
            int64_t cnt = n - off;
            if (cnt > (int64_t)(sizeof(buf) / sizeof(buf[0])))
                cnt = (int64_t)(sizeof(buf) / sizeof(buf[0]));
            for (int64_t i = 0; i < cnt; i++) {
                ray_t* s = ray_sym_vec_cell(col, off + i);
                if (!s) return RAY_ERR_CORRUPT;
                int64_t pos = ray_sym_domain_intern(w->dom, ray_str_ptr(s),
                                                    ray_str_len(s));
                if (pos < 0) return RAY_ERR_OOM;
                buf[i] = (uint32_t)pos;
            }
            if (fwrite(buf, sizeof(uint32_t), (size_t)cnt, w->fp) != (size_t)cnt)
                return RAY_ERR_IO;
            off += cnt;
        }
    } else {
        uint8_t esz = ray_sym_elem_size(w->type, 0);
        size_t bytes = (size_t)n * (size_t)esz;
        if (bytes && fwrite(ray_data(col), 1, bytes, w->fp) != bytes)
            return RAY_ERR_IO;
        if (col->attrs & RAY_ATTR_HAS_NULLS) w->had_nulls = true;
    }
    w->rows += n;
    return RAY_OK;
}

static ray_err_t csv_splayed_writer_close(csv_splayed_col_writer_t* w) {
    if (!w->fp) return RAY_OK;
    ray_err_t err = RAY_OK;

    if (err == RAY_OK) {
        ray_t hdr = {0};
        hdr.type = w->type;
        hdr.attrs = w->attrs;
        hdr.len = w->rows;
        /* SYM: header rc = the symfile's count (the loader's O(1)
         * fast-reject against the FILE domain). */
        hdr.rc = (w->type == RAY_SYM)
            ? (uint32_t)ray_sym_domain_count(w->dom) : 0;
        if (w->had_nulls) hdr.attrs |= RAY_ATTR_HAS_NULLS;
        /* Stamp the on-disk format major version into `order` so the
         * streamed column file shares the exact identity ray_col_save
         * writes — the loaders validate it.  aux stays zero (it was
         * zero-initialized above) — reserved for postponed index data. */
        ray_col_stamp_format(&hdr);
        if (fseek(w->fp, 0, SEEK_SET) != 0 ||
            fwrite(&hdr, 1, 32, w->fp) != 32)
            err = RAY_ERR_IO;
    }

    if (fclose(w->fp) != 0 && err == RAY_OK) err = RAY_ERR_IO;
    w->fp = NULL;
    if (err == RAY_OK) err = ray_file_rename(w->tmp_path, w->path);
    if (err != RAY_OK) remove(w->tmp_path);
    return err;
}

static void csv_splayed_writer_abort(csv_splayed_col_writer_t* w) {
    if (w->fp) fclose(w->fp);
    w->fp = NULL;
    remove(w->tmp_path);
}

ray_err_t ray_csv_save_splayed_named_opts(const char* path, char delimiter, bool header,
                                          const int8_t* col_types_in, int32_t n_types,
                                          const int64_t* col_names_in, int32_t n_names,
                                          const char* dir, int64_t rows_per_chunk) {
    if (!path || !dir) return RAY_ERR_DOMAIN;
    if (rows_per_chunk <= 0) rows_per_chunk = CSV_PART_ROWS_DEFAULT;

    int fd = open(path, O_RDONLY);
    if (fd < 0) return RAY_ERR_IO;

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return RAY_ERR_IO;
    }
    size_t file_size = (size_t)st.st_size;

    char* buf = (char*)ray_vm_map_fd_ro(fd, file_size);
    close(fd);
    if (!buf) return RAY_ERR_IO;

#ifdef __APPLE__
    madvise(buf, file_size, MADV_SEQUENTIAL);
#endif

    const char* buf_end = buf + file_size;
    ray_err_t err = RAY_OK;

    if (delimiter == 0) {
        int commas = 0, tabs = 0;
        for (const char* q = buf; q < buf_end && *q != '\n'; q++) {
            if (*q == ',') commas++;
            if (*q == '\t') tabs++;
        }
        delimiter = (tabs > commas) ? '\t' : ',';
    }

    int ncols = 1;
    {
        const char* q = buf;
        bool in_quote = false;
        while (q < buf_end && (in_quote || (*q != '\n' && *q != '\r'))) {
            if (*q == '"') in_quote = !in_quote;
            else if (!in_quote && *q == delimiter) ncols++;
            q++;
        }
    }
    if (ncols > CSV_MAX_COLS) {
        ray_vm_unmap_file(buf, file_size);
        return RAY_ERR_RANGE;
    }

    const char* p = buf;
    char esc_buf[8192];
    int64_t col_name_ids[CSV_MAX_COLS];

    if (header) {
        for (int c = 0; c < ncols; c++) {
            const char* fld;
            size_t flen;
            char* dyn_esc = NULL;
            p = scan_field(p, buf_end, delimiter, &fld, &flen, esc_buf, &dyn_esc);
            col_name_ids[c] = ray_sym_intern(fld, flen);
            if (dyn_esc) ray_sys_free(dyn_esc);
        }
        if (p < buf_end && *p == '\r') p++;
        if (p < buf_end && *p == '\n') p++;
    } else if (col_names_in && n_names >= ncols) {
        for (int c = 0; c < ncols; c++)
            col_name_ids[c] = col_names_in[c];
        const char* after = csv_skip_matching_header(buf, buf_end, delimiter,
                                                     ncols, col_names_in, esc_buf);
        if (after) p = after;   /* file carried a header matching the names */
    } else {
        for (int c = 0; c < ncols; c++) {
            char name[32];
            snprintf(name, sizeof(name), "V%d", c + 1);
            col_name_ids[c] = ray_sym_intern(name, strlen(name));
        }
    }

    size_t data_offset = (size_t)(p - buf);
    bool data_has_quotes = memchr(buf + data_offset, '"', file_size - data_offset) != NULL;
    int8_t resolved_types[CSV_MAX_COLS];
    if (col_types_in && n_types >= ncols) {
        for (int c = 0; c < ncols; c++) {
            int8_t t = col_types_in[c];
            if (t < RAY_BOOL ||
                (t >= RAY_TYPE_COUNT && t != RAY_CSV_AUTO_TAG) ||
                t == RAY_TABLE) {
                ray_vm_unmap_file(buf, file_size);
                return RAY_ERR_TYPE;
            }
            resolved_types[c] = t;
        }
    } else if (!col_types_in) {
        ray_t* sample_offsets_hdr = NULL;
        int64_t* sample_offsets = NULL;
        int64_t sample_n = build_row_offsets_limited(buf, file_size, data_offset,
                                                     CSV_SAMPLE_ROWS,
                                                     data_has_quotes,
                                                     &sample_offsets,
                                                     &sample_offsets_hdr,
                                                     NULL);
        bool infer_ok = csv_infer_types_from_offsets(
            buf, buf_end, sample_offsets, sample_n, ncols, delimiter,
            esc_buf, resolved_types);
        scratch_free(sample_offsets_hdr);
        if (!infer_ok) {
            ray_vm_unmap_file(buf, file_size);
            return RAY_ERR_OOM;
        }
    } else {
        ray_vm_unmap_file(buf, file_size);
        return RAY_ERR_TYPE;
    }

    for (int c = 0; c < ncols; c++) {
        if (resolved_types[c] == RAY_STR) {
            ray_t* tbl = ray_read_csv_named_opts(path, delimiter, header,
                                                 col_types_in, n_types,
                                                 col_names_in, n_names);
            if (!tbl || RAY_IS_ERR(tbl)) {
                ray_vm_unmap_file(buf, file_size);
                return tbl ? ray_err_from_obj(tbl) : RAY_ERR_IO;
            }
            /* Splay save owns the symfile now: dir/.sym is the table's
             * domain (distinct-merge + position encoding); symbol-free
             * tables write none.  The dotfile name leaves "sym" free as a
             * user column. */
            char sym_path[1024];
            int n = snprintf(sym_path, sizeof(sym_path), "%s/.sym", dir);
            if (n < 0 || (size_t)n >= sizeof(sym_path)) {
                ray_release(tbl);
                ray_vm_unmap_file(buf, file_size);
                return RAY_ERR_RANGE;
            }
            err = ray_splay_save_bulk(tbl, dir, sym_path);
            ray_release(tbl);
            ray_vm_unmap_file(buf, file_size);
            return err;
        }
    }

    /* `INT` schema columns: resolve to a concrete narrowest width over the
     * whole file before opening the fixed-width streaming column writers. */
    csv_resolve_auto_streamed(buf, file_size, data_offset, ncols, delimiter,
                              data_has_quotes, resolved_types);

    err = ray_mkdir_p(dir);
    if (err != RAY_OK) {
        ray_vm_unmap_file(buf, file_size);
        return err;
    }

    /* The .d schema is the commit marker and is written LAST (crash-safe
     * ordering: symfile → columns → .d; see ray_splay_save).  Remove any
     * pre-existing .d up front: the streaming writer renames column files
     * into place one by one, so a stale .d from a previous run could pair
     * an old schema with a partial mix of new columns — a *corrupt* table.
     * Without a .d the dir reads as a *missing* table until commit. */
    char schema_path[1024];
    int sn = snprintf(schema_path, sizeof(schema_path), "%s/.d", dir);
    if (sn < 0 || (size_t)sn >= sizeof(schema_path)) {
        ray_vm_unmap_file(buf, file_size);
        return RAY_ERR_RANGE;
    }
    remove(schema_path); /* best-effort; ENOENT is the common case */

    /* SYM columns encode against the table's symfile domain (dir/.sym):
     * open-or-create it up front; cells intern as they stream. */
    struct ray_sym_domain_s* sym_dom = NULL;
    {
        bool any_sym = false;
        for (int c = 0; c < ncols; c++)
            if (resolved_types[c] == RAY_SYM) any_sym = true;
        if (any_sym) {
            char sym_path[1024];
            int n = snprintf(sym_path, sizeof(sym_path), "%s/.sym", dir);
            if (n < 0 || (size_t)n >= sizeof(sym_path)) {
                ray_vm_unmap_file(buf, file_size);
                return RAY_ERR_RANGE;
            }
            sym_dom = ray_sym_domain_open_or_create(sym_path);
            if (!sym_dom) {
                ray_vm_unmap_file(buf, file_size);
                return RAY_ERR_IO;
            }
            /* Empty-vocabulary seeding: a header-only CSV streams no
             * cells, and ray_sym_domain_flush no-ops at count ==
             * disk_count (0 == 0 for a freshly created domain) — no
             * symfile would be written and the table would be
             * unloadable.  Seed the position-0 "" so the flush below
             * always persists a count>=1 file for SYM tables. */
            if (ray_sym_domain_count(sym_dom) == 0 &&
                ray_sym_domain_intern(sym_dom, "", 0) != 0) {
                ray_sym_domain_release(sym_dom);
                ray_vm_unmap_file(buf, file_size);
                return RAY_ERR_OOM;
            }
        }
    }

    csv_splayed_col_writer_t writers[CSV_MAX_COLS];
    memset(writers, 0, sizeof(writers));
    for (int c = 0; c < ncols; c++) {
        err = csv_splayed_writer_open(&writers[c], dir, col_name_ids[c],
                                      resolved_types[c], sym_dom);
        if (err != RAY_OK) {
            for (int j = 0; j < c; j++) csv_splayed_writer_abort(&writers[j]);
            if (sym_dom) ray_sym_domain_release(sym_dom);
            ray_vm_unmap_file(buf, file_size);
            return err;
        }
    }

    size_t chunk_offset = data_offset;
    bool wrote_any = false;
    while (chunk_offset < file_size || !wrote_any) {
        ray_t* row_offsets_hdr = NULL;
        int64_t* row_offsets = NULL;
        size_t next_offset = chunk_offset;
        int64_t cnt = 0;
        if (chunk_offset < file_size) {
            cnt = build_row_offsets_limited(buf, file_size, chunk_offset,
                                            rows_per_chunk, data_has_quotes,
                                            &row_offsets,
                                            &row_offsets_hdr, &next_offset);
            if (cnt <= 0) {
                scratch_free(row_offsets_hdr);
                err = RAY_ERR_IO;
                break;
            }
        }

        ray_t* tbl = csv_materialize_rows(buf, file_size, row_offsets,
                                          cnt, ncols, delimiter, col_name_ids,
                                          resolved_types);
        scratch_free(row_offsets_hdr);
        if (!tbl || RAY_IS_ERR(tbl)) {
            if (tbl) ray_release(tbl);
            err = RAY_ERR_OOM;
            break;
        }

        for (int c = 0; c < ncols; c++) {
            ray_t* col = ray_table_get_col_idx(tbl, c);
            err = csv_splayed_writer_append(&writers[c], col);
            if (err != RAY_OK) break;
        }
        ray_release(tbl);
        if (err != RAY_OK) break;
        wrote_any = true;
        if (cnt == 0) break;
        chunk_offset = next_offset;
    }

    /* Flush the symfile BEFORE committing column files (writer_close
     * renames tmp → final): columns must never reference positions the
     * symfile doesn't persist (sym-first crash ordering). */
    if (err == RAY_OK && sym_dom)
        err = ray_sym_domain_flush(sym_dom, false);

    for (int c = 0; c < ncols; c++) {
        ray_err_t cerr = (err == RAY_OK) ? csv_splayed_writer_close(&writers[c])
                                         : RAY_ERR_IO;
        if (err == RAY_OK && cerr != RAY_OK) err = cerr;
        if (err != RAY_OK) csv_splayed_writer_abort(&writers[c]);
    }

    /* .d LAST — the commit marker.  All column files are renamed into
     * place by now and the symfile is flushed; a crash or error before
     * this point leaves a dir without .d, which loads as a *missing*
     * table, never a corrupt one. */
    if (err == RAY_OK) {
        ray_t* schema = ray_vec_new(RAY_STR, ncols > 0 ? ncols : 1);
        if (!schema || RAY_IS_ERR(schema)) {
            if (schema) ray_release(schema);
            schema = NULL;
            err = RAY_ERR_OOM;
        }
        for (int c = 0; schema && c < ncols; c++) {
            ray_t* na = ray_sym_str(col_name_ids[c]);
            if (na)
                schema = ray_str_vec_append(schema, ray_str_ptr(na),
                                            ray_str_len(na));
            if (!na || !schema || RAY_IS_ERR(schema)) {
                if (schema && RAY_IS_ERR(schema)) ray_release(schema);
                schema = NULL;
                err = RAY_ERR_OOM;
            }
        }
        if (schema) {
            err = ray_col_save_bulk(schema, schema_path);
            ray_release(schema);
        }
    }

    if (sym_dom) ray_sym_domain_release(sym_dom);
    ray_vm_unmap_file(buf, file_size);
    return err;
}

ray_err_t ray_csv_save_parted_named_opts(const char* path, char delimiter, bool header,
                                         const int8_t* col_types_in, int32_t n_types,
                                         const int64_t* col_names_in, int32_t n_names,
                                         const char* root, const char* table_name,
                                         int64_t rows_per_part) {
    if (!path || !root || !table_name) return RAY_ERR_DOMAIN;
    if (rows_per_part <= 0) rows_per_part = CSV_PART_ROWS_DEFAULT;
    bool trace = getenv("RAY_CSV_TRACE") != NULL;

    int fd = open(path, O_RDONLY);
    if (fd < 0) return RAY_ERR_IO;

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return RAY_ERR_IO;
    }
    size_t file_size = (size_t)st.st_size;

    char* buf = (char*)ray_vm_map_fd_ro(fd, file_size);
    close(fd);
    if (!buf) return RAY_ERR_IO;

#ifdef __APPLE__
    madvise(buf, file_size, MADV_SEQUENTIAL);
#endif

    const char* buf_end = buf + file_size;
    ray_err_t err = RAY_OK;

    if (delimiter == 0) {
        int commas = 0, tabs = 0;
        for (const char* q = buf; q < buf_end && *q != '\n'; q++) {
            if (*q == ',') commas++;
            if (*q == '\t') tabs++;
        }
        delimiter = (tabs > commas) ? '\t' : ',';
    }

    int ncols = 1;
    {
        const char* q = buf;
        bool in_quote = false;
        while (q < buf_end && (in_quote || (*q != '\n' && *q != '\r'))) {
            if (*q == '"') in_quote = !in_quote;
            else if (!in_quote && *q == delimiter) ncols++;
            q++;
        }
    }
    if (ncols > CSV_MAX_COLS) {
        ray_vm_unmap_file(buf, file_size);
        return RAY_ERR_RANGE;
    }

    const char* p = buf;
    char esc_buf[8192];
    int64_t col_name_ids[CSV_MAX_COLS];

    if (header) {
        for (int c = 0; c < ncols; c++) {
            const char* fld;
            size_t flen;
            char* dyn_esc = NULL;
            p = scan_field(p, buf_end, delimiter, &fld, &flen, esc_buf, &dyn_esc);
            col_name_ids[c] = ray_sym_intern(fld, flen);
            if (dyn_esc) ray_sys_free(dyn_esc);
        }
        if (p < buf_end && *p == '\r') p++;
        if (p < buf_end && *p == '\n') p++;
    } else if (col_names_in && n_names >= ncols) {
        for (int c = 0; c < ncols; c++)
            col_name_ids[c] = col_names_in[c];
        const char* after = csv_skip_matching_header(buf, buf_end, delimiter,
                                                     ncols, col_names_in, esc_buf);
        if (after) p = after;   /* file carried a header matching the names */
    } else {
        for (int c = 0; c < ncols; c++) {
            char name[32];
            snprintf(name, sizeof(name), "V%d", c + 1);
            col_name_ids[c] = ray_sym_intern(name, strlen(name));
        }
    }

    size_t data_offset = (size_t)(p - buf);
    bool data_has_quotes = memchr(buf + data_offset, '"', file_size - data_offset) != NULL;
    if (trace) {
        fprintf(stderr,
                "csv.parted: file=%s size=%zu ncols=%d data_offset=%zu rows_per_part=%" PRId64 " root=%s table=%s\n",
                path, file_size, ncols, data_offset, rows_per_part, root, table_name);
    }

    int8_t resolved_types[CSV_MAX_COLS];
    if (col_types_in && n_types >= ncols) {
        for (int c = 0; c < ncols; c++) {
            int8_t t = col_types_in[c];
            if (t < RAY_BOOL ||
                (t >= RAY_TYPE_COUNT && t != RAY_CSV_AUTO_TAG) ||
                t == RAY_TABLE) {
                ray_vm_unmap_file(buf, file_size);
                return RAY_ERR_TYPE;
            }
            resolved_types[c] = t;
        }
    } else if (!col_types_in) {
        ray_t* sample_offsets_hdr = NULL;
        int64_t* sample_offsets = NULL;
        int64_t sample_n = build_row_offsets_limited(buf, file_size, data_offset,
                                                     CSV_SAMPLE_ROWS,
                                                     data_has_quotes,
                                                     &sample_offsets,
                                                     &sample_offsets_hdr,
                                                     NULL);
        bool infer_ok = csv_infer_types_from_offsets(
            buf, buf_end, sample_offsets, sample_n, ncols, delimiter,
            esc_buf, resolved_types);
        scratch_free(sample_offsets_hdr);
        if (!infer_ok) {
            ray_vm_unmap_file(buf, file_size);
            return RAY_ERR_OOM;
        }
    } else {
        ray_vm_unmap_file(buf, file_size);
        return RAY_ERR_TYPE;
    }

    /* `INT` schema columns: resolve to a concrete narrowest width over the
     * whole file so every partition writes the same column type. */
    csv_resolve_auto_streamed(buf, file_size, data_offset, ncols, delimiter,
                              data_has_quotes, resolved_types);

    err = ray_mkdir_p(root);
    if (err != RAY_OK) {
        ray_vm_unmap_file(buf, file_size);
        return err;
    }

    int64_t part = 0;
    size_t chunk_offset = data_offset;
    bool wrote_any = false;
    while (chunk_offset < file_size || !wrote_any) {
        ray_t* row_offsets_hdr = NULL;
        int64_t* row_offsets = NULL;
        size_t next_offset = chunk_offset;
        int64_t cnt = 0;
        if (chunk_offset < file_size) {
            cnt = build_row_offsets_limited(buf, file_size, chunk_offset,
                                            rows_per_part, data_has_quotes,
                                            &row_offsets,
                                            &row_offsets_hdr, &next_offset);
            if (cnt <= 0) {
                if (trace)
                    fprintf(stderr, "csv.parted: row-offset failure part=%" PRId64 " offset=%zu\n",
                            part, chunk_offset);
                scratch_free(row_offsets_hdr);
                err = RAY_ERR_IO;
                break;
            }
        }

        ray_t* tbl = csv_materialize_rows(buf, file_size, row_offsets,
                                          cnt, ncols, delimiter, col_name_ids, resolved_types);
        if (!tbl || RAY_IS_ERR(tbl)) {
            if (tbl) ray_release(tbl);
            scratch_free(row_offsets_hdr);
            err = RAY_ERR_OOM;
            if (trace)
                fprintf(stderr, "csv.parted: materialize failure part=%" PRId64 " rows=%" PRId64 "\n",
                        part, cnt);
            break;
        }

        char leaf[1024];
        int n = snprintf(leaf, sizeof(leaf), "%s/%" PRId64 "/%s", root, part, table_name);
        if (n < 0 || (size_t)n >= sizeof(leaf)) {
            ray_release(tbl);
            scratch_free(row_offsets_hdr);
            err = RAY_ERR_RANGE;
            break;
        }

        if (trace)
            fprintf(stderr, "csv.parted: save part=%" PRId64 " rows=%" PRId64 " leaf=%s\n",
                    part, cnt, leaf);
        /* Every partition encodes against the parted root's shared
         * symfile (root/.sym) — one domain for the whole table. */
        char root_sym[1024];
        int sn = snprintf(root_sym, sizeof(root_sym), "%s/.sym", root);
        if (sn < 0 || (size_t)sn >= sizeof(root_sym)) {
            ray_release(tbl);
            scratch_free(row_offsets_hdr);
            err = RAY_ERR_RANGE;
            break;
        }
        err = ray_splay_save_bulk(tbl, leaf, root_sym);
        ray_release(tbl);
        scratch_free(row_offsets_hdr);
        if (err != RAY_OK) {
            if (trace)
                fprintf(stderr, "csv.parted: save failure part=%" PRId64 " err=%s\n",
                        part, ray_err_code_str(err));
            break;
        }
        wrote_any = true;
        if (cnt == 0) break;
        chunk_offset = next_offset;
        part++;
    }

    /* root/.sym is maintained per-partition by ray_splay_save_bulk
     * (distinct-merge + flush before each partition's columns) — no
     * whole-dictionary dump at the end anymore. */
    ray_vm_unmap_file(buf, file_size);
    if (trace)
        fprintf(stderr, "csv.parted: done err=%s\n", ray_err_code_str(err));
    return err;
}

/* --------------------------------------------------------------------------
 * ray_read_csv — convenience wrapper with default options
 * -------------------------------------------------------------------------- */

ray_t* ray_read_csv(const char* path) {
    return ray_read_csv_opts(path, 0, true, NULL, 0);
}

/* ============================================================================
 * ray_write_csv — Write a table to a CSV file (RFC 4180)
 *
 * Writes header row with column names, then data rows.
 * Strings containing commas, quotes, or newlines are quoted.
 * Returns RAY_OK on success, error code on failure.
 * ============================================================================ */

/* -----------------------------------------------------------------------------
 * write-csv writer state
 *
 * Wraps FILE* with a sticky error flag so the dispatch loop can stay flat
 * and still report the first I/O error.  On any write failure subsequent
 * writes are skipped and the final ray_write_csv returns RAY_ERR_IO.
 * --------------------------------------------------------------------------- */

typedef struct csv_writer_t {
    FILE*     fp;
    int       err;  /* 0 = OK, non-zero = sticky error */
} csv_writer_t;

static inline void cw_putc(csv_writer_t* w, int c) {
    if (w->err) return;
    if (fputc(c, w->fp) == EOF) w->err = 1;
}

static inline void cw_write(csv_writer_t* w, const char* s, size_t len) {
    if (w->err || len == 0) return;
    if (fwrite(s, 1, len, w->fp) != len) w->err = 1;
}

static inline void cw_puts(csv_writer_t* w, const char* s) {
    if (!s) return;
    cw_write(w, s, strlen(s));
}

/* bounded, error-propagating fprintf replacement */
static void cw_printf(csv_writer_t* w, const char* fmt, ...) {
    if (w->err) return;
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) { w->err = 1; return; }
    if ((size_t)n >= sizeof(buf)) { w->err = 1; return; }
    cw_write(w, buf, (size_t)n);
}

/* Write a string value, quoting if it contains special chars */
static void csv_write_str(csv_writer_t* w, const char* s, size_t len) {
    int need_quote = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == ',' || s[i] == '"' || s[i] == '\n' || s[i] == '\r') {
            need_quote = 1;
            break;
        }
    }
    if (need_quote) {
        cw_putc(w, '"');
        size_t start = 0;
        for (size_t i = 0; i < len; i++) {
            if (s[i] == '"') {
                cw_write(w, s + start, i - start);
                cw_putc(w, '"');   /* escaped quote */
                start = i;
            }
        }
        cw_write(w, s + start, len - start);
        cw_putc(w, '"');
    } else {
        cw_write(w, s, len);
    }
}

static void csv_write_date(csv_writer_t* w, int32_t v) {
    /* days since 2000-01-01 → YYYY-MM-DD, civil_from_days (Hinnant) */
    int32_t z = v + 10957 + 719468;
    int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097);
    uint32_t yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
    int32_t  y = (int32_t)yoe + era * 400;
    uint32_t doy = doe - (365*yoe + yoe/4 - yoe/100);
    uint32_t mp = (5*doy + 2) / 153;
    int32_t  d = (int32_t)(doy - (153*mp + 2)/5 + 1);
    int32_t  m = (int32_t)(mp < 10 ? mp + 3 : mp - 9);
    if (m <= 2) y++;
    cw_printf(w, "%04d-%02d-%02d", y, m, d);
}

static void csv_write_time(csv_writer_t* w, int32_t ms) {
    /* RAY_TIME is a signed ms-of-day. Negative values represent
     * negative durations (Rayforce convention); render them
     * with a leading "-" and the absolute magnitude rather than
     * wrapping modulo one day, which would lose the sign. */
    int32_t sign = ms < 0 ? -1 : 1;
    /* Absolute value: handle INT32_MIN by widening. */
    uint32_t u = (ms == INT32_MIN) ? (uint32_t)INT32_MAX + 1u : (uint32_t)(sign == -1 ? -ms : ms);
    uint32_t h    = u / 3600000u;
    uint32_t mi   = (u % 3600000u) / 60000u;
    uint32_t s    = (u % 60000u)   / 1000u;
    uint32_t frac = u % 1000u;
    if (sign == -1) cw_putc(w, '-');
    if (frac) cw_printf(w, "%02u:%02u:%02u.%03u", h, mi, s, frac);
    else      cw_printf(w, "%02u:%02u:%02u", h, mi, s);
}

static void csv_write_timestamp(csv_writer_t* w, int64_t ns) {
    /* RAY_TIMESTAMP stores *nanoseconds* since 2000-01-01, matching
     * the language-level formatter (src/lang/format.c:ts_to_parts).
     * Splitting with C's truncating / and % rounds toward zero, so
     * fix up after the fact for negative values. */
    const int64_t NS_PER_DAY = 86400000000000LL;
    int64_t days   = ns / NS_PER_DAY;
    int64_t ns_in  = ns % NS_PER_DAY;
    if (ns_in < 0) { days--; ns_in += NS_PER_DAY; }
    /* int64 ns / NS_PER_DAY is bounded by ±~106,752 days above INT32,
     * so even INT64_MIN fits once converted to days. Still, use
     * int64 through csv_write_date by taking the low bits — any
     * timestamp that actually fits in an int64 ns count produces a
     * days value well within int32 range (~±5.88M years). */
    csv_write_date(w, (int32_t)days);
    cw_putc(w, 'T');
    uint64_t tns  = (uint64_t)ns_in;
    uint32_t h    = (uint32_t)(tns / 3600000000000ULL);
    uint32_t mi   = (uint32_t)((tns % 3600000000000ULL) / 60000000000ULL);
    uint32_t s    = (uint32_t)((tns % 60000000000ULL)   / 1000000000ULL);
    uint32_t frac = (uint32_t)(tns % 1000000000ULL);
    if (frac) cw_printf(w, "%02u:%02u:%02u.%09u", h, mi, s, frac);
    else      cw_printf(w, "%02u:%02u:%02u", h, mi, s);
}

static void csv_write_f64(csv_writer_t* w, double v) {
    if (isnan(v)) { cw_puts(w, "nan"); return; }
    if (isinf(v)) { cw_puts(w, v < 0 ? "-inf" : "inf"); return; }
    /* %.17g is the standard round-trip format; wrap in cw_printf so
     * a 64-byte buffer stack overflow guards the write. */
    cw_printf(w, "%.17g", v);
}

static void csv_write_guid(csv_writer_t* w, const uint8_t* g) {
    /* RFC 4122 canonical: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx */
    cw_printf(w,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        g[0], g[1], g[2],  g[3],  g[4],  g[5],  g[6],  g[7],
        g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}

/* Per-column resolution: slice-aware data pointer, base row offset,
 * underlying parent (for ray_vec_is_null), and a cached null flag. */
typedef struct csv_col_info_t {
    ray_t*        col;            /* original column (may be sliced) */
    ray_t*        data_owner;     /* slice_parent or col */
    int64_t       base_row;       /* slice_offset or 0 */
    const void*   data;           /* ray_data(data_owner) */
    int8_t        type;
    uint8_t       attrs;          /* of data_owner */
    bool          has_nulls;      /* requires per-row ray_vec_is_null probe */
} csv_col_info_t;

static void csv_col_info_init(csv_col_info_t* ci, ray_t* col) {
    ci->col        = col;
    ci->data_owner = col;
    ci->base_row   = 0;
    if (col && (col->attrs & RAY_ATTR_SLICE) && col->slice_parent) {
        ci->data_owner = col->slice_parent;
        ci->base_row   = col->slice_offset;
    }
    ci->type  = col ? col->type : 0;
    ci->attrs = ci->data_owner ? ci->data_owner->attrs : 0;
    ci->data  = ci->data_owner ? ray_data(ci->data_owner) : NULL;
    /* has_nulls must consult the slice_parent, since a slice view
     * never carries its own null bitmap — ray_vec_is_null handles the
     * redirect but we still want a fast bypass when neither has nulls. */
    ci->has_nulls = false;
    if (col && (col->attrs & RAY_ATTR_HAS_NULLS)) ci->has_nulls = true;
    if (ci->data_owner && (ci->data_owner->attrs & RAY_ATTR_HAS_NULLS))
        ci->has_nulls = true;
}

static void csv_write_cell(csv_writer_t* w, const csv_col_info_t* ci, int64_t r) {
    if (!ci->col) return;
    /* Null cell -> empty field (consistent with read-csv). */
    if (ci->has_nulls && ray_vec_is_null(ci->col, r)) return;

    int64_t dr = ci->base_row + r;
    int8_t t   = ci->type;
    const void* d = ci->data;

    switch (t) {
    case RAY_I64: case RAY_TIMESTAMP: break; /* handled below */
    default: break;
    }

    switch (t) {
    case RAY_I64:
        cw_printf(w, "%" PRId64, ((const int64_t*)d)[dr]);
        break;
    case RAY_I32:
        cw_printf(w, "%" PRId32, ((const int32_t*)d)[dr]);
        break;
    case RAY_I16:
        cw_printf(w, "%d", (int)((const int16_t*)d)[dr]);
        break;
    case RAY_BOOL:
        cw_puts(w, ((const uint8_t*)d)[dr] ? "true" : "false");
        break;
    case RAY_U8:
        cw_printf(w, "%u", (unsigned)((const uint8_t*)d)[dr]);
        break;
    case RAY_F64:
        csv_write_f64(w, ((const double*)d)[dr]);
        break;
    case RAY_F32:
        csv_write_f64(w, (double)((const float*)d)[dr]);
        break;
    case RAY_DATE:
        csv_write_date(w, ((const int32_t*)d)[dr]);
        break;
    case RAY_TIME:
        csv_write_time(w, ((const int32_t*)d)[dr]);
        break;
    case RAY_TIMESTAMP:
        csv_write_timestamp(w, ((const int64_t*)d)[dr]);
        break;
    case RAY_SYM: {
        /* cell-data: resolve through the column's domain (data_owner is
         * the slice parent when sliced — same data, same dictionary). */
        ray_t* s = ray_sym_vec_cell(ci->data_owner, dr);
        if (s) csv_write_str(w, ray_str_ptr(s), ray_str_len(s));
        /* unknown sym id -> empty field rather than a phantom value */
        break;
    }
    case RAY_STR: {
        /* ray_str_vec_get accepts the original (possibly sliced) col and
         * resolves the parent+offset internally.  It returns NULL for
         * nulls, which we already filtered above, so treat NULL as
         * empty-but-valid (e.g. a 0-length inline string). */
        size_t slen = 0;
        const char* sp = ray_str_vec_get(ci->col, r, &slen);
        csv_write_str(w, sp ? sp : "", slen);
        break;
    }
    case RAY_GUID:
        csv_write_guid(w, (const uint8_t*)d + dr * 16);
        break;
    case RAY_LIST: {
        /* LIST cells: recursively format each element as a string via
         * the atom's printable representation.  For nested tables /
         * lists-of-lists this produces a best-effort flat string; the
         * whole list field is quoted to keep commas inside from
         * breaking column alignment.  A LIST element is itself a
         * ray_t*, so reuse ray_fmt to get a string form. */
        ray_t** elems = (ray_t**)d;
        ray_t* e = elems[dr];
        if (!e || RAY_IS_ERR(e)) return;
        ray_t* fmt = ray_fmt(e, false);
        if (!fmt || RAY_IS_ERR(fmt)) return;
        csv_write_str(w, ray_str_ptr(fmt), ray_str_len(fmt));
        ray_release(fmt);
        break;
    }
    default:
        /* Unhandled type: emit an empty field rather than corrupting
         * downstream columns.  Callers can inspect the file and see
         * the missing data explicitly. */
        break;
    }
}

ray_err_t ray_write_csv(ray_t* table, const char* path) {
    if (!table || !path || path[0] == '\0') return RAY_ERR_TYPE;

    int64_t ncols = ray_table_ncols(table);
    int64_t nrows = ray_table_nrows(table);
    if (ncols <= 0) return RAY_ERR_TYPE;

    /* Crash-safe atomic write: tmp -> fsync -> rename. Mirrors
     * ray_col_save so an interrupted write never replaces the
     * destination with a partial file. */
    char tmp_path[1024];
    if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path) >= (int)sizeof(tmp_path))
        return RAY_ERR_IO;

    FILE* fp = fopen(tmp_path, "wb");
    if (!fp) return RAY_ERR_IO;

    csv_writer_t w = { .fp = fp, .err = 0 };

    /* Resolve every column once (slice parent, nullability, type) so
     * the hot loop just indexes into pre-computed pointers. */
    ray_t* col_info_block = ray_alloc((size_t)ncols * sizeof(csv_col_info_t));
    if (!col_info_block || RAY_IS_ERR(col_info_block)) {
        fclose(fp);
        remove(tmp_path);
        return RAY_ERR_OOM;
    }
    csv_col_info_t* ci = (csv_col_info_t*)ray_data(col_info_block);
    for (int64_t c = 0; c < ncols; c++)
        csv_col_info_init(&ci[c], ray_table_get_col_idx(table, c));

    /* Header row: column names */
    for (int64_t c = 0; c < ncols; c++) {
        if (c > 0) cw_putc(&w, ',');
        int64_t name_id = ray_table_col_name(table, c);
        ray_t* name_atom = ray_sym_str(name_id);
        if (name_atom)
            csv_write_str(&w, ray_str_ptr(name_atom), ray_str_len(name_atom));
    }
    cw_putc(&w, '\n');

    /* Data rows */
    for (int64_t r = 0; r < nrows && !w.err; r++) {
        for (int64_t c = 0; c < ncols; c++) {
            if (c > 0) cw_putc(&w, ',');
            csv_write_cell(&w, &ci[c], r);
        }
        cw_putc(&w, '\n');
    }

    ray_free(col_info_block);

    /* Flush user-space buffer before fsync/rename. */
    if (fflush(fp) != 0) w.err = 1;
    int close_err = (fclose(fp) != 0);
    if (close_err) w.err = 1;

    if (w.err) {
        remove(tmp_path);
        return RAY_ERR_IO;
    }

    /* fsync the temp file so the rename is backed by durable bytes. */
    ray_fd_t fd = ray_file_open(tmp_path, RAY_OPEN_READ | RAY_OPEN_WRITE);
    if (fd == RAY_FD_INVALID) { remove(tmp_path); return RAY_ERR_IO; }
    ray_err_t sync_err = ray_file_sync(fd);
    ray_file_close(fd);
    if (sync_err != RAY_OK) { remove(tmp_path); return sync_err; }

    ray_err_t rn_err = ray_file_rename(tmp_path, path);
    if (rn_err != RAY_OK) { remove(tmp_path); return rn_err; }

    return RAY_OK;
}
