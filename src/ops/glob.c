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
