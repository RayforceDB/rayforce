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
 * syscmd.h — single registry of system-level commands.
 *
 * One source of truth feeding three entry points:
 *
 *   1. `.sys.cmd "name args"`  — string-dispatched Rayfall builtin
 *   2. `:name args`            — REPL terminal command
 *   3. `(.sys.<name> arg)`     — direct typed Rayfall builtin (per entry)
 *
 * Each handler is invoked with one Rayfall arg (or RAY_NULL_OBJ for
 * commands that take none) and an optional REPL context that carries
 * the surface-specific state (color flag, repl pointer for things like
 * `:clear` and `:q`).  Handlers parse / coerce the arg themselves so
 * callers don't have to special-case whether `:t 1` came in as the
 * string "1" or the integer 1.
 *
 * Unknown command names dispatched through `.sys.cmd` fall through to
 * the host shell via system(2) — a shell escape, like a `system "..."`
 * call, so existing muscle memory works.
 */

#ifndef RAY_LANG_SYSCMD_H
#define RAY_LANG_SYSCMD_H

#include <rayforce.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ray_repl;

typedef struct ray_syscmd_ctx {
    struct ray_repl *repl;   /* non-NULL when invoked from the REPL */
    bool             color;  /* terminal supports ANSI; only meaningful with `repl` */
} ray_syscmd_ctx_t;

/* Handler contract:
 *   - `arg` is RAY_NULL_OBJ when no argument was supplied; otherwise an
 *     owned reference the caller manages (handlers do not retain).
 *   - Returning NULL means "no value" (treated as RAY_NULL_OBJ by the
 *     Rayfall surface; suppressed from REPL print).
 *   - Errors are returned as ray_error(...) values.
 */
typedef ray_t* (*ray_syscmd_handler_t)(ray_t* arg, ray_syscmd_ctx_t* ctx);

/* Entry flags */
#define RAY_SYSCMD_REPL_ONLY   0x01  /* not exposed via .sys.cmd / .sys.<name> */
#define RAY_SYSCMD_RESTRICTED  0x02  /* honors --restricted IPC mode */

typedef struct ray_syscmd {
    const char*          name;       /* primary command name, e.g. "timeit" */
    const char*          alias;      /* short alias e.g. "t"; NULL if none */
    ray_syscmd_handler_t fn;
    int                  flags;
    const char*          help;       /* one-line help text */
} ray_syscmd_t;

/* Look up a command by name or alias.  `name_len` lets the caller pass
 * an unterminated slice (e.g. straight out of the REPL or .sys.cmd
 * tokeniser).  Returns NULL if not found. */
const ray_syscmd_t* ray_syscmd_lookup(const char* name, size_t name_len);

/* The exit code a shell would report for a wait(2)-style status from
 * system() or pclose(): the code of a normal exit, 128 + the signal number
 * for a command a signal killed.  .sys.exec and .sys.cmd's shell fallback. */
int64_t ray_shell_exit_code(int status);

/* Walk the table.  Returns the entry array + count. */
const ray_syscmd_t* ray_syscmd_table(size_t* out_count);

/* Parse `"name args..."` into (command, arg-string), look the command
 * up, dispatch.  Unknown names fall through to system(str) when
 * `allow_shell` is set (the .sys.cmd path uses true; the REPL passes
 * false so `:foo` doesn't accidentally exec arbitrary shell). */
ray_t* ray_syscmd_dispatch(const char* str, size_t len,
                           ray_syscmd_ctx_t* ctx, bool allow_shell);

#ifdef __cplusplus
}
#endif

#endif /* RAY_LANG_SYSCMD_H */
