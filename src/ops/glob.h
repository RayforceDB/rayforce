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

#endif /* RAY_OPS_GLOB_H */
