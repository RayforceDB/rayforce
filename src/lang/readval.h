#ifndef RAY_READVAL_H
#define RAY_READVAL_H
/*
 * readval.h -- data-only reader for the text `repr` writes.
 *
 * ray_read_value turns text into one value without evaluating anything.  The
 * grammar is closed: literals, `[...]` typed vector literals, `{k: v}`
 * dictionary sugar and the four constructors list, dict, table and as.  A
 * name, any other call, an assignment or a lambda is refused, and rebinding
 * `list` or `as` in the environment changes nothing.  See
 * docs/docs/language/read-value.md for the grammar, identity rules and limits.
 */
#include <rayforce.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    size_t   max_input;         /* encoded input bytes                         */
    size_t   max_depth;         /* nesting depth, the root value is depth 1    */
    size_t   max_nodes;         /* data occurrences: scalars, elements, containers */
    size_t   max_grammar;       /* syntax nodes: tokens, calls, type tags      */
    size_t   max_symbol_bytes;  /* decoded bytes of every symbol occurrence    */
    size_t   max_alloc;         /* peak live bytes the reader itself owns      */
    uint32_t max_symbols;       /* host ceiling on the total intern count      */
    size_t   inject_fail;       /* test seam: fail the nth ledger acquisition (1-based), 0 = never */
} ray_read_limits_t;

/* The documented defaults.  max_depth is also capped at RAY_READ_DEPTH_CAP,
 * the recursion the reader will ever permit. */
#define RAY_READ_DEPTH_CAP 1024
ray_read_limits_t ray_read_limits_default(void);

/* Reads exactly one value from text[0..len).  Returns an owned value, or an
 * error: "parse" (with the byte offset), "limit" (naming the budget), "domain"
 * (a value the data model refuses) or "oom".  A failed read leaves no new
 * symbols behind.  A NULL `limits` means the defaults. */
ray_t* ray_read_value(const char* text, size_t len, const ray_read_limits_t* limits);

#endif /* RAY_READVAL_H */
