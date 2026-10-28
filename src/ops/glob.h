/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 */

#ifndef RAY_OPS_GLOB_H
#define RAY_OPS_GLOB_H

#include <stdbool.h>
#include <stddef.h>

/* Glob pattern match, iterative two-pointer (no catastrophic backtracking).
 * Worst case O(n*m); typical case linear.
 *
 * Supported metacharacters:
 *   *        — matches zero or more characters
 *   ?        — matches exactly one character
 *   [abc]    — character class: matches any of a, b, c
 *   [a-z]    — range
 *   [!abc]   — negated class
 *
 * Matching a literal metacharacter — there is no backslash escape; wrap
 * the character in a one-element class instead:
 *   [*]      matches a literal '*'
 *   [?]      matches a literal '?'
 *   [[]      matches a literal '['
 *   []]      matches a literal ']'  (']' as first char inside [...] is literal)
 *   [-]      matches a literal '-'  (as the sole char, no range to form)
 *
 * `glob_match` is case-sensitive.  `glob_match_ci` lowercases ASCII letters
 * on both sides before comparing (so it matches 'A' against 'a', 'A-Z'
 * range matches both case forms, etc.).
 *
 * Lenient parsing policy: an unterminated character class (e.g. pattern
 * "abc[def" with no closing `]`) is accepted — the class consumes input
 * up to the end of the pattern and the match continues with whatever
 * `matched` flag accumulated.  This matches glibc fnmatch's permissive
 * behaviour and avoids surprising `error: parse` mid-search.  Callers
 * that want strict validation should pre-validate the pattern. */
bool ray_glob_match(const char* s, size_t sn, const char* p, size_t pn);
bool ray_glob_match_ci(const char* s, size_t sn, const char* p, size_t pn);

/* ---- Pre-compiled pattern fast path -------------------------------------
 * Many LIKE workloads have very simple patterns (e.g. `*google*`).  When
 * the pattern has no metacharacters except (optionally) a leading `*`
 * and/or a trailing `*`, the match collapses to a literal substring /
 * prefix / suffix / equality test that we can drive with memcmp /
 * memmem — both libc-vectorised on modern glibc.  Detect the shape once
 * up front, then run the entire dictionary (or row vector) through a
 * single tight loop.
 *
 * Shapes:
 *   RAY_GLOB_SHAPE_NONE     — pattern needs the full glob matcher
 *   RAY_GLOB_SHAPE_EXACT    — no `*`/`?`/`[` — literal equality
 *   RAY_GLOB_SHAPE_PREFIX   — `<lit>*`        — strncmp prefix
 *   RAY_GLOB_SHAPE_SUFFIX   — `*<lit>`        — tail equality
 *   RAY_GLOB_SHAPE_CONTAINS — `*<lit>*`       — memmem
 *   RAY_GLOB_SHAPE_ANY      — pattern is "*"  — always true
 * The compiled struct caches a pointer/length into the original
 * pattern buffer, so the caller must keep the pattern alive while the
 * compiled view is in use. */
typedef enum {
    RAY_GLOB_SHAPE_NONE = 0,
    RAY_GLOB_SHAPE_EXACT,
    RAY_GLOB_SHAPE_PREFIX,
    RAY_GLOB_SHAPE_SUFFIX,
    RAY_GLOB_SHAPE_CONTAINS,
    RAY_GLOB_SHAPE_ANY,
} ray_glob_shape_t;

typedef struct {
    ray_glob_shape_t shape;
    const char*      lit;     /* literal substring inside the pattern */
    size_t           lit_len;
} ray_glob_compiled_t;

/* Classify a pattern.  Returns the simplest matching shape; falls back
 * to RAY_GLOB_SHAPE_NONE when the pattern needs the general matcher. */
ray_glob_compiled_t ray_glob_compile(const char* p, size_t pn);

/* Match a single string against a compiled simple-shape pattern.
 * Caller must guarantee shape != RAY_GLOB_SHAPE_NONE. */
bool ray_glob_match_compiled(const ray_glob_compiled_t* c,
                             const char* s, size_t sn);

typedef struct {
    const char* pat;
    size_t      pat_len;
    size_t      tokens;
    const char* anchor;
    size_t      anchor_len;
    size_t      anchor_token;
    bool        literal;
} ray_strpat_t;

/* Compile a search pattern using literals, `?`, and character classes.
 * `*` is not a search wildcard; match a literal star with `[*]`.
 * (str-find does not use this: it is a literal search, ray_str_find_literal.) */
bool ray_strpat_compile(const char* p, size_t pn, ray_strpat_t* out);

/* Find the first byte offset whose substring matches a search pattern.
 * The empty pattern matches at offset 0. */
bool ray_strpat_find(const ray_strpat_t* c, const char* s, size_t sn,
                     size_t* out_pos);
/* Literal substring search (str-find): first byte offset of the needle in
 * s, no wildcards. The empty needle matches at offset 0. */
bool ray_str_find_literal(const char* s, size_t sn, const char* nd, size_t nl,
                          size_t* out_pos);
bool ray_strpat_find_raw(const char* s, size_t sn, const char* p, size_t pn,
                         size_t* out_pos);

#endif /* RAY_OPS_GLOB_H */
