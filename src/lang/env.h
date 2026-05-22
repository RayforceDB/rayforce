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

#ifndef RAY_ENV_H
#define RAY_ENV_H

#include <rayforce.h>
#include "lang/eval.h"

/* Create function objects. Name stored inline in nullmap[0..15].
 * The function pointer is in the i64 field. */
ray_t* ray_fn_unary(const char* name, uint8_t fn_attrs, ray_unary_fn fn);
ray_t* ray_fn_binary(const char* name, uint8_t fn_attrs, ray_binary_fn fn);
ray_t* ray_fn_vary(const char* name, uint8_t fn_attrs, ray_vary_fn fn);

/* Read builtin name from nullmap[2..15] (null-terminated, max 13 chars).
 * Bytes 0-1 reserved for DAG opcode on all function types. */
static inline const char* ray_fn_name(const ray_t* fn) {
    return (const char*)fn->nullmap + 2;
}

/* Global environment: symbol -> function object dict */
ray_err_t ray_env_init(void);
void     ray_env_destroy(void);
ray_t*    ray_env_get(int64_t sym_id);

/* User-facing binder.  Refuses any name starting with `.` — that root is
 * reserved for system namespaces (.sys, .os, .io, .ipc, …) populated by
 * builtin registration.  Returns RAY_ERR_RESERVED in that case. */
ray_err_t ray_env_set(int64_t sym_id, ray_t* val);

/* Internal binder used by builtin registration.  Identical to ray_env_set
 * but WITHOUT the reserved-namespace guard.  Do NOT call this from user-
 * exposed paths; it is the intended way to populate `.sys` / `.os` etc.
 * during ray_lang_init. */
ray_err_t ray_env_bind(int64_t sym_id, ray_t* val);

/* Flat variant of ray_env_bind: writes the binding directly into the
 * global env hash without traversing dotted-segment dict upserts.
 * Used to register every fully-qualified builtin name (`.sys.gc`,
 * `.os.getenv`, …) alongside the root namespace dict, so prefix
 * lookup (REPL completion + highlighter) enumerates them all. */
ray_err_t ray_env_bind_flat(int64_t sym_id, ray_t* val);

/* True if a symbol's interned name starts with `.` — i.e. it belongs to
 * the reserved namespace populated at startup by builtin registration.
 * User-level binders (ray_env_set, ray_env_set_local, lambda parameter
 * installer) refuse such names so system bindings can't be shadowed. */
bool ray_sym_is_reserved(int64_t sym_id);

/* Resolve a name for a Rayfall expression (tree-walking eval or bytecode
 * op_resolve): returns an OWNED ref (rc >= 1) that the caller must
 * release, or NULL if undefined.  Unlike ray_env_get which returns a
 * borrowed ref and leaves refcount management to the caller, env_resolve
 * retains before returning — so name-resolution sites can drop their
 * manual ray_retain and still participate in the dotted-sym temporal
 * extraction path (e.g. `trades.Time.dd`), which allocates fresh values
 * mid-walk. */
ray_t*    ray_env_resolve(int64_t sym_id);

/* Prefix lookup: scan global env + keywords for names starting with prefix.
 * Fills results[] with pointers to interned name strings (valid until next
 * sym table mutation).  Returns count of matches (up to max_results).
 * Results are sorted alphabetically. */
int64_t ray_env_lookup_prefix(const char* prefix, int64_t len,
                              const char** results, int64_t max_results);

/* True iff `name[0..len)` is an exact-match global env binding or
 * keyword.  Does NOT intern the probed string (unlike ray_env_get which
 * would need a sym_id).  Used by the REPL highlighter to decide whether
 * to paint the current word green — the prefix-lookup API returns only
 * the first-matching entry, which would misclassify `de` as non-builtin
 * when an alphabetically-earlier `desc`/`del` hits the same prefix. */
bool ray_env_has_name(const char* name, int64_t len);

/* Iterate global environment entries.
 * Fills sym_ids[] and vals[] with up to max_entries items.
 * Returns count of entries written. */
int32_t ray_env_list(int64_t* sym_ids, ray_t** vals, int32_t max_entries);

/* Iterate ONLY user-defined bindings (slots last written via ray_env_set,
 * not ray_env_bind).  Powers the journal snapshot — the .qdb file would
 * otherwise carry every builtin, which is wasteful and breaks on reload
 * because builtin function objects hold absolute pointers from the prior
 * process.  A user `(set + 42)` over a builtin flips the slot to user-
 * defined, so explicit overrides are preserved. */
int32_t ray_env_list_user(int64_t* sym_ids, ray_t** vals, int32_t max_entries);

/* Total number of bindings currently in the global env (builtins +
 * user).  Useful for sizing buffers before ray_env_list. */
int32_t ray_env_global_count(void);

/* Local scope stack for lexical binding (let, do, lambda) */
ray_err_t ray_env_push_scope(void);
void ray_env_pop_scope(void);
ray_err_t ray_env_set_local(int64_t sym_id, ray_t* val);

#endif /* RAY_ENV_H */
