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

#include "lang/parse.h"
#include "lang/nfo.h"
#include "lang/env.h"
#include "core/numparse.h"
#include "table/sym.h"   /* RAY_SYM_W64 */
#include <string.h>
#include <limits.h>
#include <stdint.h>
#include <math.h>

/* ══════════════════════════════════════════
 * ASCII dispatch table (128 bytes)
 * Single indexed read: PA(c) — zero branches.
 * ══════════════════════════════════════════ */

#define PA_ERR     0
#define PA_DIGIT   1
#define PA_ALPHA   2
#define PA_STRING  3
#define PA_QUOTE   4    /* ' symbol prefix */
#define PA_LPAREN  5
#define PA_RPAREN  6
#define PA_LBRACK  7
#define PA_RBRACK  8
#define PA_LBRACE  9
#define PA_RBRACE  10
#define PA_COLON   11
#define PA_WS      12
#define PA_END     13
#define PA_MINUS   14
#define PA_SEMI    15   /* ; comment */

static const char _PA[128] =
/*  NUL                              \t \n                         */
    "\x0d\x00\x00\x00\x00\x00\x00\x00\x00\x0c\x0c\x00\x00\x0c\x00\x00"
/*                                                                  */
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
/*  SP   !    "    #    $    %    &    '    (    )    *    +    ,    -    .    /  */
    "\x0c\x02\x03\x02\x02\x02\x02\x04\x05\x06\x02\x02\x02\x0e\x02\x02"
/*  0    1    2    3    4    5    6    7    8    9    :    ;    <    =    >    ?  */
    "\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x0b\x0f\x02\x02\x02\x02"
/*  @    A    B    C    D    E    F    G    H    I    J    K    L    M    N    O  */
    "\x02\x02\x02\x02\x02\x02\x02\x02\x02\x02\x02\x02\x02\x02\x02\x02"
/*  P    Q    R    S    T    U    V    W    X    Y    Z    [    \    ]    ^    _  */
    "\x02\x02\x02\x02\x02\x02\x02\x02\x02\x02\x02\x07\x00\x08\x02\x02"
/*  `    a    b    c    d    e    f    g    h    i    j    k    l    m    n    o  */
    "\x00\x02\x02\x02\x02\x02\x02\x02\x02\x02\x02\x02\x02\x02\x02\x02"
/*  p    q    r    s    t    u    v    w    x    y    z    {    |    }    ~   DEL */
    "\x02\x02\x02\x02\x02\x02\x02\x02\x02\x02\x02\x09\x02\x0a\x02\x00";

#define PA(c) ((unsigned char)(c) < 128 ? (int)(unsigned char)_PA[(unsigned char)(c)] : PA_ERR)

/* ══════════════════════════════════════════
 * Parser state
 * ══════════════════════════════════════════ */

typedef struct {
    const char *src;
    const char *pos;
    int32_t line;
    int32_t col;
    ray_t  *nfo;
} ray_parser_t;

static void advance(ray_parser_t *p, int32_t n) {
    for (int32_t i = 0; i < n; i++) {
        if (p->pos[i] == '\n') { p->line++; p->col = 0; }
        else { p->col++; }
    }
    p->pos += n;
}

/* Fixup line/col after raw p->pos advancement (scan consumed region). */
static void fixup_pos(ray_parser_t *p, const char *old_pos) {
    for (const char *c = old_pos; c < p->pos; c++) {
        if (*c == '\n') { p->line++; p->col = 0; }
        else { p->col++; }
    }
}

/* Record a span for node in the nfo object. */
static void nfo_record(ray_parser_t *p, ray_t *node,
                        int32_t sl, int32_t sc) {
    if (!p->nfo || RAY_IS_ERR(node)) return;
    ray_span_t span;
    span.start_line = (uint16_t)sl;
    span.start_col  = (uint16_t)sc;
    span.end_line   = (uint16_t)p->line;
    span.end_col    = (uint16_t)(p->col > 0 ? p->col - 1 : 0);
    ray_nfo_insert(p->nfo, node, span);
}

static void skip_ws_and_comments(ray_parser_t *p) {
    for (;;) {
        while (*p->pos == ' ' || *p->pos == '\t' || *p->pos == '\n' || *p->pos == '\r') {
            if (*p->pos == '\n') { p->line++; p->col = 0; }
            else { p->col++; }
            p->pos++;
        }
        if (*p->pos == ';') {
            while (*p->pos && *p->pos != '\n') { p->col++; p->pos++; }
            continue;
        }
        break;
    }
}

/* Forward declarations */
static ray_t* parse_expr(ray_parser_t *p);

/* ── Date/time/timestamp helpers ── */

#include "lang/cal.h"

#define PARSE_NSECS_IN_DAY ((int64_t)24 * 60 * 60 * 1000000000LL)

/* Try to parse a time literal starting from 'start'.
 * Returns the char past the end on success, NULL on failure.
 * Writes the millisecond value into *ms_out, including sign. */
static const char* try_parse_time(const char* start, int32_t *ms_out) {
    const char* c = start;
    int sign = 1;
    if (*c == '-') { sign = -1; c++; }

    /* HH */
    if (!(c[0] >= '0' && c[0] <= '9' && c[1] >= '0' && c[1] <= '9')) return NULL;
    int hh = (c[0] - '0') * 10 + (c[1] - '0'); c += 2;
    if (*c != ':') return NULL;
    c++;

    /* MM */
    if (!(c[0] >= '0' && c[0] <= '9' && c[1] >= '0' && c[1] <= '9')) return NULL;
    int mm = (c[0] - '0') * 10 + (c[1] - '0'); c += 2;
    if (*c != ':') return NULL;
    c++;

    /* SS */
    if (!(c[0] >= '0' && c[0] <= '9' && c[1] >= '0' && c[1] <= '9')) return NULL;
    int ss = (c[0] - '0') * 10 + (c[1] - '0'); c += 2;

    /* .mmm (milliseconds) */
    int ms = 0;
    if (*c == '.') {
        c++;
        if (!(*c >= '0' && *c <= '9')) return NULL;
        ms = (*c - '0'); c++;
        if (*c >= '0' && *c <= '9') { ms = ms * 10 + (*c - '0'); c++; }
        if (*c >= '0' && *c <= '9') { ms = ms * 10 + (*c - '0'); c++; }
    }

    *ms_out = sign * (int32_t)((hh * 3600 + mm * 60 + ss) * 1000 + ms);
    return c;
}

/* ── Number parsing (with hex, nulls, typed suffixes, date/time/timestamp) ── */
static ray_t* parse_number(ray_parser_t *p) {
    const char *start = p->pos;
    int is_neg = 0;
    if (*p->pos == '-') { is_neg = 1; p->pos++; }

    /* Hex literal: 0x.. */
    if (p->pos[0] == '0' && p->pos[1] == 'x') {
        p->pos += 2;
        uint64_t v;
        size_t n = ray_parse_u64_hex(p->pos, SIZE_MAX, &v);
        if (n == 0) return ray_error("parse", NULL);
        p->pos += n;
        return ray_u8((uint8_t)v);
    }

    /* Null literal: 0N{h,i,d,t,p,l,f,s} or bare 0N (defaults to i64 null). */
    if (!is_neg && p->pos[0] == '0' && p->pos[1] == 'N') {
        switch (p->pos[2]) {
        case 'h': p->pos += 3; return ray_typed_null(-RAY_I16);
        case 'i': p->pos += 3; return ray_typed_null(-RAY_I32);
        case 'd': p->pos += 3; return ray_typed_null(-RAY_DATE);
        case 't': p->pos += 3; return ray_typed_null(-RAY_TIME);
        case 'p': p->pos += 3; return ray_typed_null(-RAY_TIMESTAMP);
        case 'l': p->pos += 3; return ray_typed_null(-RAY_I64);
        case 'f': p->pos += 3; return ray_typed_null(-RAY_F64);
        case 's': p->pos += 3; return ray_typed_null(-RAY_SYM);
        }
        /* Bare 0N: only if the next char is not an identifier continuation
         * (letter/digit/underscore), else fall through to plain number. */
        char c2 = p->pos[2];
        if (!((c2 >= 'a' && c2 <= 'z') || (c2 >= 'A' && c2 <= 'Z') ||
              (c2 >= '0' && c2 <= '9') || c2 == '_')) {
            p->pos += 2;
            return ray_typed_null(-RAY_I64);
        }
    }

    /* Scan digits */
    const char *dstart = p->pos;
    while (*p->pos >= '0' && *p->pos <= '9') p->pos++;
    int ndigits = (int)(p->pos - dstart);

    /* Date/Timestamp: YYYY.MM.DD or YYYY.MM.DDDhh:mm:ss.nnnnnnnnn */
    if (ndigits == 4 && !is_neg && *p->pos == '.' &&
        p->pos[1] >= '0' && p->pos[1] <= '9' &&
        p->pos[2] >= '0' && p->pos[2] <= '9' &&
        p->pos[3] == '.') {
        int year = (int)ray_parse_4_digits(dstart);
        p->pos++; /* skip first '.' */
        int month = (p->pos[0] - '0') * 10 + (p->pos[1] - '0');
        p->pos += 2;
        if (*p->pos != '.') { p->pos = start; goto plain_number; }
        p->pos++; /* skip second '.' */
        if (!(p->pos[0] >= '0' && p->pos[0] <= '9' &&
              p->pos[1] >= '0' && p->pos[1] <= '9')) {
            p->pos = start; goto plain_number;
        }
        int day = (p->pos[0] - '0') * 10 + (p->pos[1] - '0');
        p->pos += 2;

        int32_t days = ymd_to_date(year, month, day);

        /* Check for timestamp separator 'D' */
        if (*p->pos == 'D') {
            p->pos++; /* skip D */
            /* Parse HH:MM:SS.nnnnnnnnn */
            if (!(p->pos[0] >= '0' && p->pos[0] <= '9' &&
                  p->pos[1] >= '0' && p->pos[1] <= '9'))
                return ray_error("parse", NULL);
            int hh = (p->pos[0] - '0') * 10 + (p->pos[1] - '0'); p->pos += 2;
            if (*p->pos != ':') return ray_error("parse", NULL);
            p->pos++;
            int mi = (p->pos[0] - '0') * 10 + (p->pos[1] - '0'); p->pos += 2;
            if (*p->pos != ':') return ray_error("parse", NULL);
            p->pos++;
            int ss = (p->pos[0] - '0') * 10 + (p->pos[1] - '0'); p->pos += 2;
            if (*p->pos != '.') return ray_error("parse", NULL);
            p->pos++;
            /* Parse fractional seconds (up to 9 digits for nanoseconds) */
            const char* fstart = p->pos;
            while (*p->pos >= '0' && *p->pos <= '9') p->pos++;
            int flen = (int)(p->pos - fstart);
            uint64_t nanos = 0;
            for (int i = 0; i < flen && i < 9; i++)
                nanos = nanos * 10 + (uint64_t)(fstart[i] - '0');
            /* Pad to 9 digits */
            for (int i = flen; i < 9; i++) nanos *= 10;

            int64_t day_ns = (int64_t)days * PARSE_NSECS_IN_DAY;
            int64_t time_ns = ((int64_t)hh * 3600 + mi * 60 + ss) * 1000000000LL + (int64_t)nanos;
            return ray_timestamp(day_ns + time_ns);
        }

        return ray_date(days);
    }

    /* Time literal: HH:MM:SS.mmm (detected by colon after 2 digits from digit-start) */
    if (ndigits == 2 && *p->pos == ':') {
        p->pos = start; /* reset — let try_parse_time handle sign */
        int32_t ms;
        const char* end = try_parse_time(start, &ms);
        if (end) { p->pos = end; return ray_time(ms); }
        /* Not a valid time — fall through to regular number parsing */
        p->pos = start;
        if (is_neg) p->pos++;
        while (*p->pos >= '0' && *p->pos <= '9') p->pos++;
    }

plain_number:;
    /* At this point p->pos is past the digits. Check for float */
    int is_float = 0;
    if (*p->pos == '.' && p->pos[1] >= '0' && p->pos[1] <= '9') {
        is_float = 1;
        p->pos++;
        while (*p->pos >= '0' && *p->pos <= '9') p->pos++;
    }
    if (*p->pos == 'e' || *p->pos == 'E') {
        is_float = 1;
        p->pos++;
        if (*p->pos == '+' || *p->pos == '-') p->pos++;
        while (*p->pos >= '0' && *p->pos <= '9') p->pos++;
    }

    size_t span = (size_t)(p->pos - start);

    if (is_float) {
        double v = 0.0;
        if (ray_parse_f64(start, span, &v) == 0)
            return ray_error("parse", NULL);
        return ray_f64(v);
    }

    /* Integer parse — overflow signalled by `n == 0` (digits present but
     * value didn't fit int64).  Promote to f64 in that case, matching the
     * historical strtoll/ERANGE → strtod behavior. */
    int64_t v = 0;
    size_t n = ray_parse_i64(start, span, &v);
    if (n == 0) {
        double fv = 0.0;
        if (ray_parse_f64(start, span, &fv) == 0)
            return ray_error("parse", NULL);
        return ray_f64(fv);
    }

    /* Type suffix: h (i16), i (i32) */
    if (*p->pos == 'h') {
        p->pos++;
        if (v < -32767 || v > 32767) return ray_error("domain", NULL);
        return ray_i16((int16_t)v);
    }
    if (*p->pos == 'i') {
        p->pos++;
        if (v < -2147483647LL || v > 2147483647LL) return ray_error("domain", NULL);
        return ray_i32((int32_t)v);
    }

    return ray_i64(v);
}

/* ── String parsing with escape sequence decoding ── */
static ray_t* parse_string(ray_parser_t *p) {
    p->pos++; /* skip opening " */
    const char *start = p->pos;

    /* First pass: scan for closing " and check for escapes */
    bool has_escape = false;
    const char *scan = p->pos;
    while (*scan && *scan != '"') {
        if (*scan == '\\' && scan[1]) { has_escape = true; scan++; }
        scan++;
    }
    size_t raw_len = (size_t)(scan - start);
    if (*scan != '"') return ray_error("parse", NULL); /* unterminated string */
    scan++;
    p->pos = scan;

    if (!has_escape) return ray_str(start, raw_len);

    /* Decode escape sequences into a temporary buffer */
    char buf[4096];
    size_t out = 0;
    const char *r = start;
    const char *end = start + raw_len;
    while (r < end) {
        if (out >= sizeof(buf) - 2)
            return ray_error("domain", NULL);  /* string too long for escape buffer */
        if (*r == '\\' && r + 1 < end) {
            r++;
            switch (*r) {
            case 'n':  buf[out++] = '\n'; r++; break;
            case 't':  buf[out++] = '\t'; r++; break;
            case 'r':  buf[out++] = '\r'; r++; break;
            case '\\': buf[out++] = '\\'; r++; break;
            case '"':  buf[out++] = '"';  r++; break;
            case '0': case '1': case '2': case '3':
            case '4': case '5': case '6': case '7': {
                /* Octal escape: \OOO (1-3 digits) */
                char ch = (char)(*r - '0'); r++;
                if (r < end && *r >= '0' && *r <= '7') {
                    ch = (char)((ch << 3) | (*r - '0')); r++;
                    if (r < end && *r >= '0' && *r <= '7') {
                        ch = (char)((ch << 3) | (*r - '0')); r++;
                    }
                }
                buf[out++] = ch;
                break;
            }
            default:   buf[out++] = '\\'; buf[out++] = *r; r++; break;
            }
        } else {
            buf[out++] = *r++;
        }
    }
    return ray_str(buf, out);
}

/* ── Symbol/char parsing: 'name or 'a' ── */
static ray_t* parse_symbol(ray_parser_t *p) {
    p->pos++; /* skip ' */
    const char *start = p->pos;

    /* Empty symbol (bare tick at end or before terminator) */
    if (*p->pos == 0 || *p->pos == ' ' || *p->pos == '\t' || *p->pos == '\n' ||
        *p->pos == ')' || *p->pos == ']' || *p->pos == '}') {
        /* Null symbol 0Ns */
        return ray_typed_null(-RAY_SYM);
    }

    /* Char literal: 'X' or '\n' etc. */
    if (*p->pos == '\\') {
        /* Escape sequence char literal */
        const char *esc = p->pos + 1;
        char ch;
        int esc_len = 1;
        switch (*esc) {
        case 'n':  ch = '\n'; break;
        case 'r':  ch = '\r'; break;
        case 't':  ch = '\t'; break;
        case '\\': ch = '\\'; break;
        case '\'': ch = '\''; break;
        case '0': case '1': case '2': case '3':
        case '4': case '5': case '6': case '7': {
            /* Octal escape: \OOO */
            ch = (char)(*esc - '0');
            if (esc[1] >= '0' && esc[1] <= '7') {
                ch = (char)((ch << 3) | (esc[1] - '0'));
                if (esc[2] >= '0' && esc[2] <= '7') {
                    ch = (char)((ch << 3) | (esc[2] - '0'));
                    esc_len = 3;
                } else {
                    esc_len = 2;
                }
            }
            break;
        }
        default: ch = *esc; break;
        }
        if (esc[esc_len] == '\'') {
            /* Closing quote found — it's a char literal */
            p->pos = esc + esc_len + 1;
            return ray_str(&ch, 1);
        }
        /* Not a char literal — fall through to symbol parsing */
    } else if (start[1] == '\'') {
        /* Simple char literal like 'a' */
        char ch = *start;
        p->pos = start + 2; /* skip char + closing quote */
        return ray_str(&ch, 1);
    }

    /* Regular symbol */
    while (PA(*p->pos) == PA_ALPHA || PA(*p->pos) == PA_DIGIT || *p->pos == '_' || *p->pos == '.')
        p->pos++;
    size_t len = (size_t)(p->pos - start);
    if (len == 0) return ray_typed_null(-RAY_SYM); /* empty symbol */
    int64_t id = ray_sym_intern(start, len);
    return ray_sym(id);
}

/* ── Name parsing ── */
static ray_t* parse_name(ray_parser_t *p) {
    const char *start = p->pos;
    /* Name chars: alpha, digit, _, ., -, !, ?, +, *, /, %, <, >, =, & */
    while (PA(*p->pos) == PA_ALPHA || PA(*p->pos) == PA_DIGIT
           || *p->pos == '_' || *p->pos == '.' || *p->pos == '-'
           || *p->pos == '!' || *p->pos == '?' || *p->pos == '+'
           || *p->pos == '*' || *p->pos == '/' || *p->pos == '%'
           || *p->pos == '<' || *p->pos == '>' || *p->pos == '='
           || *p->pos == '&' || *p->pos == '|')
        p->pos++;
    size_t len = (size_t)(p->pos - start);
    if (len == 0) return ray_error("parse", NULL);

    /* Check for true/false */
    if (len == 4 && memcmp(start, "true", 4) == 0)  return ray_bool(true);
    if (len == 5 && memcmp(start, "false", 5) == 0) return ray_bool(false);
    /* null is handled as a name that resolves to NULL at eval time */

    /* Return as name symbol (with RAY_ATTR_NAME flag) */
    int64_t id = ray_sym_intern(start, len);
    ray_t* s = ray_sym(id);
    if (!RAY_IS_ERR(s)) s->attrs |= RAY_ATTR_NAME;
    return s;
}

/* ── Vector literal: [1 2 3] ── */
static ray_t* parse_vector(ray_parser_t *p) {
    advance(p, 1); /* skip [ */

    /* Collect parsed elements into a temporary array */
    ray_t* elems[4096];
    int32_t count = 0;

    skip_ws_and_comments(p);
    while (*p->pos && *p->pos != ']') {
        if (count >= 4096) {
            for (int32_t i = 0; i < count; i++) ray_release(elems[i]);
            return ray_error("limit", NULL);
        }
        ray_t* elem = parse_expr(p);
        if (RAY_IS_ERR(elem)) {
            for (int32_t i = 0; i < count; i++) ray_release(elems[i]);
            return elem;
        }
        elems[count++] = elem;
        skip_ws_and_comments(p);
    }
    if (*p->pos != ']') {
        for (int32_t i = 0; i < count; i++) ray_release(elems[i]);
        return ray_error("parse", NULL);
    }
    advance(p, 1); /* skip ] */

    if (count == 0) {
        /* Empty vector -> empty i64 vector */
        return ray_vec_new(RAY_I64, 0);
    }

    /* Determine element types.
     * Name references (RAY_ATTR_NAME) must stay as boxed atoms because
     * the evaluator, compiler, and fn-builder dereference them as ray_t*. */
    int8_t first_type = elems[0]->type;
    bool homogeneous = true;
    bool has_float = (first_type == -RAY_F64);
    bool has_int   = (first_type == -RAY_I64);
    bool all_numeric = (first_type == -RAY_I64 || first_type == -RAY_F64);

    for (int32_t i = 0; i < count; i++) {
        /* Inside [...], names are symbol literals, not variable references */
        if (elems[i]->attrs & RAY_ATTR_NAME) {
            elems[i]->attrs &= ~RAY_ATTR_NAME;
            /* type is already -RAY_SYM from parse_expr */
        }
        if (i == 0) continue;
        int8_t t = elems[i]->type;
        if (t != first_type) homogeneous = false;
        if (t == -RAY_F64)      has_float = true;
        else if (t == -RAY_I64) has_int = true;
        if (t != -RAY_I64 && t != -RAY_F64) all_numeric = false;
    }

    /* All same atom type -> typed vector */
    if (homogeneous && first_type < 0) {
        int8_t vec_type = -first_type;
        ray_t* vec = ray_vec_new(vec_type, count);
        if (RAY_IS_ERR(vec)) {
            for (int32_t i = 0; i < count; i++) ray_release(elems[i]);
            return vec;
        }
        switch (vec_type) {
            case RAY_I64: case RAY_TIMESTAMP: {
                int64_t* d = (int64_t*)ray_data(vec);
                for (int32_t i = 0; i < count; i++) d[i] = elems[i]->i64;
                break;
            }
            case RAY_F64: {
                double* d = (double*)ray_data(vec);
                for (int32_t i = 0; i < count; i++) d[i] = elems[i]->f64;
                break;
            }
            case RAY_I32: case RAY_DATE: case RAY_TIME: {
                int32_t* d = (int32_t*)ray_data(vec);
                for (int32_t i = 0; i < count; i++) d[i] = elems[i]->i32;
                break;
            }
            case RAY_I16: {
                int16_t* d = (int16_t*)ray_data(vec);
                for (int32_t i = 0; i < count; i++) d[i] = elems[i]->i16;
                break;
            }
            case RAY_BOOL: {
                bool* d = (bool*)ray_data(vec);
                for (int32_t i = 0; i < count; i++) d[i] = elems[i]->b8;
                break;
            }
            case RAY_SYM: {
                int64_t* d = (int64_t*)ray_data(vec);
                for (int32_t i = 0; i < count; i++) d[i] = elems[i]->i64;
                break;
            }
            case RAY_U8: {
                uint8_t* d = (uint8_t*)ray_data(vec);
                for (int32_t i = 0; i < count; i++) d[i] = elems[i]->u8;
                break;
            }
            case RAY_STR: {
                /* String vectors use ray_str_vec_append */
                ray_t* svec = ray_vec_new(RAY_STR, count);
                if (RAY_IS_ERR(svec)) {
                    ray_free(vec);
                    for (int32_t i = 0; i < count; i++) ray_release(elems[i]);
                    return svec;
                }
                for (int32_t i = 0; i < count; i++) {
                    const char* s = ray_str_ptr(elems[i]);
                    size_t slen = ray_str_len(elems[i]);
                    svec = ray_str_vec_append(svec, s, slen);
                    if (RAY_IS_ERR(svec)) {
                        for (int32_t j = 0; j < count; j++) ray_release(elems[j]);
                        ray_free(vec);
                        return svec;
                    }
                }
                ray_free(vec);
                for (int32_t i = 0; i < count; i++) ray_release(elems[i]);
                return svec;
            }
            default: ray_free(vec); goto boxed_list;
        }
        vec->len = count;
        for (int32_t i = 0; i < count; i++) {
            if (RAY_ATOM_IS_NULL(elems[i]))
                ray_vec_set_null(vec, i, true);
            ray_release(elems[i]);
        }
        return vec;
    }

    /* Mixed int/float -> promote to f64 */
    if (has_float && has_int && all_numeric) {
        ray_t* vec = ray_vec_new(RAY_F64, count);
        if (RAY_IS_ERR(vec)) {
            for (int32_t i = 0; i < count; i++) ray_release(elems[i]);
            return vec;
        }
        double* d = (double*)ray_data(vec);
        for (int32_t i = 0; i < count; i++) {
            d[i] = (elems[i]->type == -RAY_F64) ? elems[i]->f64
                                                 : (double)elems[i]->i64;
        }
        vec->len = count;
        for (int32_t i = 0; i < count; i++) {
            if (RAY_ATOM_IS_NULL(elems[i]))
                ray_vec_set_null(vec, i, true);
            ray_release(elems[i]);
        }
        return vec;
    }

boxed_list:
    /* Mixed types in vector literal — domain error */
    for (int32_t i = 0; i < count; i++) ray_release(elems[i]);
    return ray_error("domain", NULL);
}

/* ── Dict literal: {key: val key: val ...} ──
 *
 * Builds a RAY_DICT block holding [keys, vals].
 * Keys are emitted as a RAY_SYM vector when every key is a bareword sym
 * literal, as a RAY_STR vector when every key is a quoted string literal,
 * or as a heterogeneous RAY_LIST otherwise.  Values stay unevaluated in
 * a RAY_LIST so dict literals remain self-evaluating (the (dict ...)
 * builtin evaluates them on demand).
 */
static ray_t* parse_dict(ray_parser_t *p) {
    advance(p, 1); /* skip { */

    /* Build keys+vals as a generic RAY_LIST of atoms first; then narrow
     * keys to a typed vector if homogeneous.  16 entries cover every
     * realistic dict literal — heterogeneous spillover stays as LIST. */
    ray_t* key_list = ray_list_new(8);
    if (RAY_IS_ERR(key_list)) return key_list;
    ray_t* vals = ray_list_new(8);
    if (RAY_IS_ERR(vals)) { ray_release(key_list); return vals; }

    bool all_sym = true;
    bool all_str = true;

    skip_ws_and_comments(p);
    while (*p->pos && *p->pos != '}') {
        ray_t* key_atom = NULL;
        if (*p->pos == '"') {
            const char *sk_before = p->pos;
            key_atom = parse_string(p);
            fixup_pos(p, sk_before);
            if (RAY_IS_ERR(key_atom)) { ray_release(key_list); ray_release(vals); return key_atom; }
            all_sym = false;
        } else {
            const char *kstart = p->pos;
            while (PA(*p->pos) == PA_ALPHA || PA(*p->pos) == PA_DIGIT
                   || *p->pos == '_' || *p->pos == '-')
                p->pos++;
            p->col += (int32_t)(p->pos - kstart);
            size_t klen = (size_t)(p->pos - kstart);
            if (klen == 0) { ray_release(key_list); ray_release(vals); return ray_error("parse", NULL); }
            int64_t kid = ray_sym_intern(kstart, klen);
            key_atom = ray_sym(kid);
            if (RAY_IS_ERR(key_atom)) { ray_release(key_list); ray_release(vals); return key_atom; }
            all_str = false;
        }

        skip_ws_and_comments(p);
        if (*p->pos != ':') { ray_release(key_atom); ray_release(key_list); ray_release(vals); return ray_error("parse", NULL); }
        advance(p, 1);
        skip_ws_and_comments(p);

        ray_t* val = parse_expr(p);
        if (RAY_IS_ERR(val)) { ray_release(key_atom); ray_release(key_list); ray_release(vals); return val; }

        key_list = ray_list_append(key_list, key_atom);
        ray_release(key_atom);
        if (RAY_IS_ERR(key_list)) { ray_release(vals); ray_release(val); return key_list; }

        vals = ray_list_append(vals, val);
        ray_release(val);
        if (RAY_IS_ERR(vals)) { ray_release(key_list); return vals; }

        skip_ws_and_comments(p);
    }
    if (*p->pos != '}') { ray_release(key_list); ray_release(vals); return ray_error("parse", NULL); }
    advance(p, 1); /* skip } */

    /* Narrow keys to a typed vector when homogeneous. */
    int64_t n_pairs = key_list->len;
    ray_t** key_atoms = (ray_t**)ray_data(key_list);
    ray_t* keys;
    if (n_pairs > 0 && all_sym) {
        keys = ray_sym_vec_new(RAY_SYM_W64, n_pairs);
        if (RAY_IS_ERR(keys)) { ray_release(key_list); ray_release(vals); return keys; }
        for (int64_t i = 0; i < n_pairs; i++) {
            int64_t id = key_atoms[i]->i64;
            keys = ray_vec_append(keys, &id);
            if (RAY_IS_ERR(keys)) { ray_release(key_list); ray_release(vals); return keys; }
        }
        ray_release(key_list);
    } else if (n_pairs > 0 && all_str) {
        keys = ray_vec_new(RAY_STR, n_pairs);
        if (RAY_IS_ERR(keys)) { ray_release(key_list); ray_release(vals); return keys; }
        for (int64_t i = 0; i < n_pairs; i++) {
            keys = ray_str_vec_append(keys, ray_str_ptr(key_atoms[i]), ray_str_len(key_atoms[i]));
            if (RAY_IS_ERR(keys)) { ray_release(key_list); ray_release(vals); return keys; }
        }
        ray_release(key_list);
    } else {
        keys = key_list;  /* heterogeneous or empty — use the LIST as-is */
    }
    return ray_dict_new(keys, vals);
}

/* ── List (s-expression): (fn arg1 arg2 ...) ── */
static ray_t* parse_list(ray_parser_t *p) {
    advance(p, 1); /* skip ( */
    ray_t* list = ray_list_new(4);
    if (RAY_IS_ERR(list)) return list;

    skip_ws_and_comments(p);
    while (*p->pos && *p->pos != ')') {
        ray_t* elem = parse_expr(p);
        if (RAY_IS_ERR(elem)) { ray_release(list); return elem; }
        list = ray_list_append(list, elem);
        ray_release(elem);
        if (RAY_IS_ERR(list)) return list;
        skip_ws_and_comments(p);
    }
    if (*p->pos != ')') { ray_release(list); return ray_error("parse", NULL); }
    advance(p, 1); /* skip ) */
    return list;
}

/* ── Main expression dispatch ── */
static ray_t* parse_expr(ray_parser_t *p) {
    skip_ws_and_comments(p);

    int32_t sl = p->line, sc = p->col;
    const char *before = p->pos;
    ray_t *result;

    switch (PA(*p->pos)) {
        case PA_END:    return ray_error("parse", NULL);
        case PA_DIGIT:  result = parse_number(p); break;
        case PA_MINUS:
            if (p->pos[1] >= '0' && p->pos[1] <= '9')
                result = parse_number(p);
            else
                result = parse_name(p);  /* standalone '-' or '-name' */
            break;
        case PA_ALPHA:  result = parse_name(p); break;
        case PA_STRING: result = parse_string(p); break;
        case PA_QUOTE:  result = parse_symbol(p); break;
        case PA_LPAREN: result = parse_list(p); break;
        case PA_LBRACK: result = parse_vector(p); break;
        case PA_LBRACE: result = parse_dict(p); break;
        case PA_RPAREN: return ray_error("parse", NULL);
        case PA_RBRACK: return ray_error("parse", NULL);
        case PA_RBRACE: return ray_error("parse", NULL);
        case PA_COLON: {
            /* Keyword literal :name — parse as symbol (like 'name) */
            p->pos++;  /* skip : */
            const char *kstart = p->pos;
            while (PA(*p->pos) == PA_ALPHA || PA(*p->pos) == PA_DIGIT
                   || *p->pos == '_' || *p->pos == '.' || *p->pos == '-'
                   || *p->pos == '/' || *p->pos == '?')
                p->pos++;
            size_t klen = (size_t)(p->pos - kstart);
            if (klen == 0) { result = ray_error("parse", "empty keyword"); break; }
            int64_t kid = ray_sym_intern(kstart, klen);
            result = ray_sym(kid);
            break;
        }
        default:        result = parse_name(p); break;  /* operators like +, *, etc. */
    }

    /* Fixup line/col: leaf parsers advance pos without updating line/col.
     * Compound parsers (list/vector/dict) use advance() internally and
     * call skip_ws_and_comments, so their line/col is already accurate. */
    if (PA(*before) != PA_LPAREN && PA(*before) != PA_LBRACK && PA(*before) != PA_LBRACE)
        fixup_pos(p, before);
    nfo_record(p, result, sl, sc);
    return result;
}

/* ── Internal parse driver (shared by public APIs) ── */
static ray_t* parse_source(ray_parser_t *p) {
    ray_t* first = parse_expr(p);
    if (RAY_IS_ERR(first)) return first;

    /* Check if there are more expressions after the first */
    skip_ws_and_comments(p);
    if (*p->pos == '\0') return first;  /* single expression */

    /* Multiple expressions: collect into (do expr1 expr2 ...) */
    ray_t* exprs[256];
    int32_t count = 0;
    exprs[count++] = first;

    while (*p->pos) {
        if (count >= 256) {
            for (int32_t i = 0; i < count; i++) ray_release(exprs[i]);
            return ray_error("domain", NULL);  /* too many top-level expressions */
        }
        ray_t* expr = parse_expr(p);
        if (RAY_IS_ERR(expr)) {
            for (int32_t i = 0; i < count; i++) ray_release(exprs[i]);
            return expr;
        }
        exprs[count++] = expr;
        skip_ws_and_comments(p);
    }

    /* Build (do expr1 expr2 ...) list */
    int32_t sl = p->line, sc = p->col;
    ray_t* do_list = ray_alloc((count + 1) * sizeof(ray_t*));
    if (!do_list) {
        for (int32_t i = 0; i < count; i++) ray_release(exprs[i]);
        return ray_error("oom", NULL);
    }
    do_list->type = RAY_LIST;
    do_list->len = 0;
    ray_t** elems = (ray_t**)ray_data(do_list);
    /* Build a name-reference atom for "do" so parsing is independent of runtime */
    ray_t* do_sym = ray_alloc(0);
    if (!do_sym) {
        ray_release(do_list);
        for (int32_t i = 0; i < count; i++) ray_release(exprs[i]);
        return ray_error("oom", NULL);
    }
    do_sym->type = -RAY_SYM;
    do_sym->attrs = RAY_ATTR_NAME;
    do_sym->i64 = ray_sym_intern("do", 2);
    elems[0] = do_sym;
    for (int32_t i = 0; i < count; i++)
        elems[i + 1] = exprs[i];
    do_list->len = count + 1;
    nfo_record(p, do_list, sl, sc);
    return do_list;
}

/* ── Public API ── */
ray_t* ray_parse(const char* source) {
    return ray_parse_with_nfo(source, NULL);
}

ray_t* ray_parse_with_nfo(const char* source, ray_t* nfo) {
    if (!source) return ray_error("parse", NULL);
    ray_parser_t p = {
        .src  = source,
        .pos  = source,
        .line = 0,
        .col  = 0,
        .nfo  = nfo
    };
    return parse_source(&p);
}
