/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 */

/*
 * Iterative glob matcher.  Replaces three pre-existing implementations
 * that diverged in syntax (eval used *,?,[abc]; DAG used SQL %,_) and
 * one of which (strop.c::str_glob) blew up exponentially on patterns
 * like "a*a*a*…a*b" against an a-only string.  This single file is
 * the only matcher; both call sites delegate here.
 */

#include "ops/glob.h"

#define _GNU_SOURCE
#include <string.h>

/* Lowercase an ASCII byte; non-ASCII passes through unchanged. */
static inline char to_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/* Match a single character against a class `[ ... ]`.  On entry *pi
 * points at the byte after `[`.  On return *pi points one past `]`.
 * Recognises `[abc]`, `[a-z]`, leading `!` for negation, embedded
 * `]` is allowed as the first char (after optional `!`). */
static bool match_class(const char* p, size_t pn, size_t* pi, char c, bool ci) {
    size_t i = *pi;
    bool neg = false;
    if (i < pn && p[i] == '!') { neg = true; i++; }
    bool matched = false;
    bool first = true;
    char ch = ci ? to_lower(c) : c;
    while (i < pn && (first || p[i] != ']')) {
        char lo = ci ? to_lower(p[i]) : p[i];
        if (i + 2 < pn && p[i + 1] == '-' && p[i + 2] != ']') {
            char hi = ci ? to_lower(p[i + 2]) : p[i + 2];
            if (ch >= lo && ch <= hi) matched = true;
            i += 3;
        } else {
            if (ch == lo) matched = true;
            i++;
        }
        first = false;
    }
    if (i < pn && p[i] == ']') i++;  /* consume closing bracket */
    *pi = i;
    return neg ? !matched : matched;
}

static bool glob_impl(const char* s, size_t sn,
                     const char* p, size_t pn, bool ci) {
    size_t si = 0, pi = 0;
    size_t star_pi = (size_t)-1, star_si = 0;

    while (si < sn) {
        if (pi < pn && p[pi] == '*') {
            star_pi = pi++;        /* remember star, skip it */
            star_si = si;
        } else if (pi < pn && p[pi] == '?') {
            pi++;
            si++;
        } else if (pi < pn && p[pi] == '[') {
            size_t cls_pi = pi + 1;
            if (match_class(p, pn, &cls_pi, s[si], ci)) {
                pi = cls_pi;
                si++;
            } else if (star_pi != (size_t)-1) {
                pi = star_pi + 1;
                si = ++star_si;
            } else {
                return false;
            }
        } else if (pi < pn) {
            char a = ci ? to_lower(s[si]) : s[si];
            char b = ci ? to_lower(p[pi]) : p[pi];
            if (a == b) {
                pi++;
                si++;
            } else if (star_pi != (size_t)-1) {
                pi = star_pi + 1;
                si = ++star_si;
            } else {
                return false;
            }
        } else if (star_pi != (size_t)-1) {
            pi = star_pi + 1;
            si = ++star_si;
        } else {
            return false;
        }
    }
    /* Consumed all of input — pattern must be at end, modulo trailing stars. */
    while (pi < pn && p[pi] == '*') pi++;
    return pi == pn;
}

bool ray_glob_match(const char* s, size_t sn, const char* p, size_t pn) {
    return glob_impl(s, sn, p, pn, false);
}

bool ray_glob_match_ci(const char* s, size_t sn, const char* p, size_t pn) {
    return glob_impl(s, sn, p, pn, true);
}

ray_glob_compiled_t ray_glob_compile(const char* p, size_t pn) {
    ray_glob_compiled_t c = { RAY_GLOB_SHAPE_NONE, NULL, 0 };

    if (pn == 0) {
        c.shape = RAY_GLOB_SHAPE_EXACT;
        c.lit = p; c.lit_len = 0;
        return c;
    }

    /* Strip a single leading and trailing '*'; classify by the residual
     * pattern.  Any other glob metachar (`?`, `[`, or interior `*`)
     * forces the general matcher. */
    size_t lo = 0, hi = pn;
    bool leading_star  = (p[0] == '*');
    bool trailing_star = (pn > 0 && p[pn - 1] == '*' &&
                          /* don't double-count single '*' as both */
                          (pn > 1 || !leading_star));
    if (leading_star)  lo = 1;
    if (trailing_star) hi = pn - 1;

    /* Ensure the residual has no glob metacharacters. */
    for (size_t i = lo; i < hi; i++) {
        char ch = p[i];
        if (ch == '*' || ch == '?' || ch == '[') {
            c.shape = RAY_GLOB_SHAPE_NONE;
            return c;
        }
    }

    c.lit     = p + lo;
    c.lit_len = hi - lo;

    if (leading_star && trailing_star) {
        c.shape = (c.lit_len == 0) ? RAY_GLOB_SHAPE_ANY
                                   : RAY_GLOB_SHAPE_CONTAINS;
    } else if (leading_star) {
        c.shape = RAY_GLOB_SHAPE_SUFFIX;
    } else if (trailing_star) {
        c.shape = RAY_GLOB_SHAPE_PREFIX;
    } else {
        c.shape = RAY_GLOB_SHAPE_EXACT;
    }
    return c;
}

bool ray_glob_match_compiled(const ray_glob_compiled_t* c,
                             const char* s, size_t sn) {
    switch (c->shape) {
    case RAY_GLOB_SHAPE_ANY:
        return true;
    case RAY_GLOB_SHAPE_EXACT:
        return sn == c->lit_len &&
               (c->lit_len == 0 || memcmp(s, c->lit, c->lit_len) == 0);
    case RAY_GLOB_SHAPE_PREFIX:
        return sn >= c->lit_len &&
               (c->lit_len == 0 || memcmp(s, c->lit, c->lit_len) == 0);
    case RAY_GLOB_SHAPE_SUFFIX:
        return sn >= c->lit_len &&
               (c->lit_len == 0 ||
                memcmp(s + sn - c->lit_len, c->lit, c->lit_len) == 0);
    case RAY_GLOB_SHAPE_CONTAINS:
        if (c->lit_len == 0) return true;
        if (sn < c->lit_len) return false;
        /* glibc's memmem is SIMD-accelerated; use it where available.
         * Falls back to a portable Boyer-Moore-Horspool when not. */
#if defined(__GLIBC__) || defined(__APPLE__) || defined(__FreeBSD__)
        return memmem(s, sn, c->lit, c->lit_len) != NULL;
#else
        {
            /* Portable fallback: short-needle byte scan with memchr. */
            const char first = c->lit[0];
            const char* haystack = s;
            size_t remaining = sn;
            while (remaining >= c->lit_len) {
                const char* hit = (const char*)memchr(haystack, first,
                                                      remaining - c->lit_len + 1);
                if (!hit) return false;
                if (memcmp(hit, c->lit, c->lit_len) == 0) return true;
                size_t adv = (size_t)(hit - haystack) + 1;
                haystack = hit + 1;
                remaining -= adv;
            }
            return false;
        }
#endif
    case RAY_GLOB_SHAPE_NONE:
    default:
        /* Caller contract violation — fall through to false rather than
         * silently matching everything. */
        return false;
    }
}

static const char* strpat_memmem(const char* s, size_t sn,
                                 const char* needle, size_t needle_len) {
    if (needle_len == 0) return s;
    if (needle_len > sn) return NULL;
    if (needle_len == 1) return (const char*)memchr(s, needle[0], sn);
#if defined(__GLIBC__) || defined(__APPLE__) || defined(__FreeBSD__)
    return (const char*)memmem(s, sn, needle, needle_len);
#else
    const char first = needle[0];
    const char* hay = s;
    size_t remaining = sn;
    while (remaining >= needle_len) {
        const char* hit = (const char*)memchr(hay, first,
                                              remaining - needle_len + 1);
        if (!hit) return NULL;
        if (memcmp(hit, needle, needle_len) == 0) return hit;
        size_t adv = (size_t)(hit - hay) + 1;
        hay = hit + 1;
        remaining -= adv;
    }
    return NULL;
#endif
}

bool ray_str_find_literal(const char* s, size_t sn, const char* nd, size_t nl,
                          size_t* out_pos) {
    const char* hit = strpat_memmem(s, sn, nd, nl);
    if (!hit) return false;
    if (out_pos) *out_pos = (size_t)(hit - s);
    return true;
}

bool ray_strpat_compile(const char* p, size_t pn, ray_strpat_t* out) {
    if (!out) return false;
    *out = (ray_strpat_t){ .pat = p, .pat_len = pn, .literal = true };

    size_t tokens = 0;
    size_t run_start = 0, run_len = 0, run_token = 0;
    size_t best_len = 0, best_start = 0, best_token = 0;

#define SEARCH_FLUSH_RUN() do {                         \
    if (run_len > best_len) {                            \
        best_len = run_len;                              \
        best_start = run_start;                          \
        best_token = run_token;                          \
    }                                                    \
    run_len = 0;                                         \
} while (0)

    for (size_t i = 0; i < pn; ) {
        if (p[i] == '*') return false;
        if (p[i] == '?' || p[i] == '[') {
            out->literal = false;
            SEARCH_FLUSH_RUN();
            if (p[i] == '[') {
                size_t j = i + 1;
                bool first = true;
                if (j < pn && p[j] == '!') j++;
                while (j < pn && (first || p[j] != ']')) {
                    if (j + 2 < pn && p[j + 1] == '-' && p[j + 2] != ']')
                        j += 3;
                    else
                        j++;
                    first = false;
                }
                if (j < pn && p[j] == ']') j++;
                i = j;
            } else {
                i++;
            }
            tokens++;
            continue;
        }

        if (run_len == 0) {
            run_start = i;
            run_token = tokens;
        }
        run_len++;
        i++;
        tokens++;
    }
    SEARCH_FLUSH_RUN();
#undef SEARCH_FLUSH_RUN

    out->tokens = tokens;
    out->anchor = p + best_start;
    out->anchor_len = best_len;
    out->anchor_token = best_token;
    return true;
}

static bool strpat_match_at(const ray_strpat_t* c,
                            const char* s, size_t sn,
                            size_t pos) {
    const char* p = c->pat;
    size_t pn = c->pat_len;
    size_t si = pos;
    for (size_t pi = 0; pi < pn; ) {
        if (si >= sn) return false;
        if (p[pi] == '?') {
            pi++;
            si++;
            continue;
        }
        if (p[pi] == '[') {
            size_t cls_pi = pi + 1;
            if (!match_class(p, pn, &cls_pi, s[si], false))
                return false;
            pi = cls_pi;
            si++;
            continue;
        }
        if (s[si] != p[pi]) return false;
        pi++;
        si++;
    }
    return true;
}

bool ray_strpat_find(const ray_strpat_t* c, const char* s, size_t sn,
                     size_t* out_pos) {
    if (out_pos) *out_pos = 0;
    if (!c) return false;
    if (c->tokens == 0) return true;
    if (c->tokens > sn) return false;

    if (c->literal) {
        const char* hit = strpat_memmem(s, sn, c->pat, c->pat_len);
        if (!hit) return false;
        if (out_pos) *out_pos = (size_t)(hit - s);
        return true;
    }

    if (c->anchor_len > 0) {
        size_t tail = c->tokens - c->anchor_token - c->anchor_len;
        size_t scan_from = c->anchor_token;
        while (scan_from + c->anchor_len + tail <= sn) {
            size_t hay_len = sn - tail - scan_from;
            const char* hit = strpat_memmem(s + scan_from, hay_len,
                                            c->anchor, c->anchor_len);
            if (!hit) return false;
            size_t anchor_pos = (size_t)(hit - s);
            size_t start = anchor_pos - c->anchor_token;
            if (start + c->tokens <= sn &&
                strpat_match_at(c, s, sn, start)) {
                if (out_pos) *out_pos = start;
                return true;
            }
            scan_from = anchor_pos + 1;
        }
        return false;
    }

    size_t last = sn - c->tokens;
    for (size_t pos = 0; pos <= last; pos++) {
        if (strpat_match_at(c, s, sn, pos)) {
            if (out_pos) *out_pos = pos;
            return true;
        }
    }
    return false;
}

bool ray_strpat_find_raw(const char* s, size_t sn, const char* p, size_t pn,
                         size_t* out_pos) {
    ray_strpat_t c;
    if (!ray_strpat_compile(p, pn, &c)) return false;
    return ray_strpat_find(&c, s, sn, out_pos);
}
