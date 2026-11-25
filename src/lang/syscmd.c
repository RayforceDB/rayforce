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

#include "lang/syscmd.h"
/* We don't need the VM internals here — only the runtime accessors
 * and the lang-side builtins.  The runtime exposes its main poll via
 * opaque-pointer accessors declared inline below.  (Historically this
 * also dodged a dual ray_vm_t typedef; that's unified in
 * core/runtime.h now, the externs just keep the TU lean.) */
#include "core/poll.h"
#include "core/ipc.h"
#include "core/profile.h"
#include "core/platform.h"   /* RAY_OS_WINDOWS */
#include "lang/env.h"
#include "lang/inspect.h"
#include "table/sym.h"

/* Forward decls of the bare runtime accessors — defined in
 * core/runtime.c; one extern keeps this TU decoupled from runtime.h. */
void* ray_runtime_get_poll(void);

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(RAY_OS_WINDOWS)
#include <sys/wait.h>   /* WIFEXITED/WEXITSTATUS: shell exit codes */
#endif

/* Turn a wait(2)-style status into the exit code a shell would report.
 * system() and pclose() hand back an encoded status, not a code: "exit 3"
 * arrives as 768 (3 << 8).  Signals follow the shell's 128+N convention, so
 * a command killed by SIGTERM reads as 143 rather than as a bare 15 that
 * could be confused with a real exit status.  .sys.exec and the shell
 * fallback of .sys.cmd both answer through it (#1740). */
int64_t ray_shell_exit_code(int status) {
#if defined(RAY_OS_WINDOWS)
    /* The CRT's system()/_pclose() already return the child's exit code. */
    return (int64_t)status;
#else
    if (WIFEXITED(status))   return (int64_t)WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return (int64_t)(128 + WTERMSIG(status));
    return (int64_t)status;
#endif
}

/* Public header reaches us through syscmd.h.  These few helpers were
 * previously sourced from lang/internal.h; pulling them in directly
 * keeps this TU clear of the runtime/eval VM clash. */
static inline int ray_is_atom_local(ray_t* x) { return x && !RAY_IS_ERR(x) && x->type < 0; }

/* ══════════════════════════════════════════
 * Argument parsing helpers — handlers receive a Rayfall ray_t* but
 * the .sys.cmd / REPL paths arrive with a raw char slice.  These
 * helpers coerce both into the typed value the handler wants,
 * keeping the per-handler code free of input-shape branching.
 * ══════════════════════════════════════════ */

static int arg_is_null(const ray_t* arg) {
    /* Not an eval/builtin value position: arg is a freshly-built syscmd
     * argument that can be a bare C NULL on deep-OOM (ray_str alloc fail) —
     * keep the !arg guard rather than RAY_ASSERT_VALUE. */
    return !arg || RAY_IS_NULL(arg);
}

/* Parse signed decimal int64 out of a ray_t (atom or string).  Returns
 * 0 + sets *err=1 if the arg can't be coerced. */
static int64_t arg_as_i64(ray_t* arg, int* err) {
    *err = 0;
    if (arg_is_null(arg)) { *err = 1; return 0; }
    if (arg->type == -RAY_I64) return arg->i64;
    if (arg->type == -RAY_I32) return (int64_t)arg->i32;
    if (arg->type == -RAY_I16) return (int64_t)arg->i16;
    if (arg->type == -RAY_U8)  return (int64_t)arg->u8;
    if (arg->type == -RAY_BOOL) return (int64_t)arg->b8;
    if (arg->type == -RAY_STR) {
        const char* p = ray_str_ptr(arg);
        size_t len = ray_str_len(arg);
        size_t i = 0;
        while (i < len && (p[i] == ' ' || p[i] == '\t')) i++;
        int sign = 1;
        if (i < len && (p[i] == '+' || p[i] == '-')) { if (p[i] == '-') sign = -1; i++; }
        if (i >= len || p[i] < '0' || p[i] > '9') { *err = 1; return 0; }
        uint64_t v = 0;
        uint64_t limit = sign < 0 ? (uint64_t)INT64_MAX + 1u : (uint64_t)INT64_MAX;
        while (i < len && p[i] >= '0' && p[i] <= '9') {
            uint64_t digit = (uint64_t)(p[i] - '0');
            if (v > (limit - digit) / 10u) { *err = 1; return 0; }
            v = v * 10u + digit;
            i++;
        }
        if (i != len) { *err = 1; return 0; }
        if (sign < 0) {
            if (v == (uint64_t)INT64_MAX + 1u) return INT64_MIN;
            return -(int64_t)v;
        }
        return (int64_t)v;
    }
    *err = 1;
    return 0;
}

/* ══════════════════════════════════════════
 * Handlers
 * ══════════════════════════════════════════ */

/* timeit/t — toggle the profiler.
 *   no arg → toggle
 *   0      → disable
 *   nonzero→ enable
 */
static ray_t* h_timeit(ray_t* arg, ray_syscmd_ctx_t* ctx) {
    bool active;
    if (arg_is_null(arg)) {
        active = !g_ray_profile.active;
    } else {
        int err = 0;
        int64_t v = arg_as_i64(arg, &err);
        if (err) return ray_error("type", ":t expects an integer (0 = off, 1 = on)");
        active = (v != 0);
    }
    g_ray_profile.active = active;
    if (ctx && ctx->repl) {
        if (ctx->color) fprintf(stdout, "\033[1;33m");
        fprintf(stdout, ". Timeit is %s.", active ? "on" : "off");
        if (ctx->color) fprintf(stdout, "\033[0m");
        fprintf(stdout, "\n");
        return NULL;
    }
    return ray_i64(active ? 1 : 0);
}

/* listen N — bind an IPC listener on PORT using the runtime's main
 * poll instance.  Errors with `nyi` if no main loop is wired (i.e.
 * the host didn't call ray_runtime_create from main.c, or libray is
 * being used as an embedded library without a poll).  Errors with
 * `io` if the bind fails (port in use, permission, etc.).  Returns
 * the listener id on success. */
static ray_t* h_listen(ray_t* arg, ray_syscmd_ctx_t* ctx) {
    (void)ctx;
#ifdef RAY_FUZZING
    /* No network listeners under fuzzing. */
    (void)arg;
    return ray_error("restricted", "listen disabled under fuzzing");
#endif
    /* Two forms: a port number binds INADDR_ANY (unchanged), and a
     * "HOST:PORT" string confines the listener to one IPv4 address
     * (#427) — e.g. (.sys.listen "127.0.0.1:7701") for loopback-only. */
    char host[64];
    const char* bind_host = NULL;
    int64_t port;
    const char* s    = (arg && arg->type == -RAY_STR) ? ray_str_ptr(arg) : NULL;
    size_t      slen = s ? ray_str_len(arg) : 0;
    const char* colon = s ? memchr(s, ':', slen) : NULL;
    if (colon) {
        if (colon == s || (size_t)(colon - s) >= sizeof(host))
            return ray_error("type", "listen expects a port number or \"HOST:PORT\"");
        memcpy(host, s, (size_t)(colon - s));
        host[colon - s] = '\0';
        bind_host = host;
        /* Strict digits-only port, the .ipc.open rule: "1abc" is a
         * malformed port, not port 1. */
        char pbuf[16];
        size_t plen = slen - (size_t)(colon - s) - 1;
        if (plen == 0 || plen >= sizeof(pbuf))
            return ray_error("domain", "listen: port out of range (1..65535)");
        memcpy(pbuf, colon + 1, plen);
        pbuf[plen] = '\0';
        char* endp;
        errno = 0;
        long pv = strtol(pbuf, &endp, 10);
        if (endp == pbuf || *endp != '\0' || errno == ERANGE)
            return ray_error("domain", "listen: port must be numeric 1..65535, got \"%s\"", pbuf);
        port = (int64_t)pv;
    } else {
        /* Port-only form: an i64/int atom, or (via the .sys.cmd string
         * dispatcher) a numeric string — arg_as_i64 handles both. */
        int err = 0;
        port = arg_as_i64(arg, &err);
        if (err) return ray_error("type", "listen expects a port number or \"HOST:PORT\"");
    }
    if (port <= 0 || port > 65535) return ray_error("domain", "listen: port out of range (1..65535)");

    ray_poll_t* poll = (ray_poll_t*)ray_runtime_get_poll();
    if (!poll) return ray_error("nyi", "listen: no main event loop attached");

    int64_t id = ray_ipc_listen_at(poll, bind_host, (uint16_t)port);
    if (id < 0) {
        int e = errno;
        return ray_error("io", "listen: bind to %s port %lld failed: %s",
                         bind_host ? bind_host : "*", (long long)port,
                         strerror(e ? e : EADDRINUSE));
    }
    return ray_i64(id);
}

/* env — list defined globals.  REPL prints a summary; Rayfall path
 * returns a list of [name, type-label] pairs. */
static const char* type_label_short(ray_t* v) {
    if (!v) return "null";
    switch (v->type) {
        case RAY_LAMBDA:  return "lambda";
        case RAY_UNARY:
        case RAY_BINARY:
        case RAY_VARY:    return "fn";
        case RAY_TABLE:   return "table";
        case RAY_DICT:    return "dict";
        case RAY_LIST:    return "list";
        default:
            if (v->type < 0) return "atom";
            if (v->type > 0) return "vec";
            return "?";
    }
}

static ray_t* h_env(ray_t* arg, ray_syscmd_ctx_t* ctx) {
    (void)arg;
    int64_t sym_ids[512];
    ray_t*  vals[512];
    int32_t n = ray_env_list(sym_ids, vals, 512);
    if (ctx && ctx->repl) {
        for (int32_t i = 0; i < n; i++) {
            ray_t* s = ray_sym_str(sym_ids[i]);
            const char* name = s ? ray_str_ptr(s) : "?";
            fprintf(stdout, "  %-20s %s\n", name, type_label_short(vals[i]));
        }
        fprintf(stdout, "(%d entries)\n", n);
        return NULL;
    }
    /* Non-REPL: just return the count.  Returning the full env as a
     * Rayfall list is doable but not needed for the .sys.cmd "env"
     * use case (which is purely informational). */
    return ray_i64(n);
}

/* clear — REPL-only screen clear. */
static ray_t* h_clear(ray_t* arg, ray_syscmd_ctx_t* ctx) {
    (void)arg;
    if (ctx && ctx->repl && ctx->color) {
        fprintf(stdout, "\033[2J\033[H");
        fflush(stdout);
    }
    return NULL;
}

/* One field of a .repl.signature dict, owned, or NULL. */
static ray_t* sig_field(ray_t* d, const char* key) {
    ray_t* k = ray_sym(ray_sym_intern(key, strlen(key)));
    if (!k || RAY_IS_ERR(k)) return NULL;
    ray_t* v = ray_dict_get(d, k);
    ray_release(k);
    return v;
}

static bool sig_flag(ray_t* d, const char* key) {
    ray_t* v = sig_field(d, key);
    bool b = v && !RAY_IS_ERR(v) && v->type == -RAY_BOOL && v->b8;
    if (v) ray_release(v);
    return b;
}

/* Prints a symbol's name. */
static void print_sym(int64_t id) {
    ray_t* s = ray_sym_str(id);
    if (s) fwrite(ray_str_ptr(s), 1, ray_str_len(s), stdout);
}

/* `:h name` — what (.repl.signature name) reports, for a person: the kind,
 * then a builtin's documented forms one per line, or a lambda's call shape
 * from its parameter names.  Nothing is evaluated or called. */
static ray_t* help_name(const char* p, size_t len, ray_syscmd_ctx_t* ctx) {
    if (len > 0 && p[0] == '\'') { p++; len--; }
    bool one = len > 0;
    for (size_t i = 0; i < len; i++)
        if (p[i] == ' ' || p[i] == '\t') one = false;
    if (!one) {
        fprintf(stdout, ". :h takes one name, e.g. :h min\n");
        return NULL;
    }
    int64_t id = ray_sym_intern(p, len);
    ray_t* name = ray_sym(id);
    if (!name || RAY_IS_ERR(name)) return name ? name : ray_error("oom", NULL);
    ray_t* d = ray_repl_signature_fn(name);
    ray_release(name);
    if (!d || RAY_IS_ERR(d)) return d ? d : ray_error("oom", NULL);

    ray_t* kind = sig_field(d, "kind");
    ray_t* usage = sig_field(d, "usage");
    ray_t* summary = sig_field(d, "summary");
    ray_t* params = sig_field(d, "params");
    ray_t* own = sig_field(d, "builtin");
    const char* k = "";
    ray_t* ks = kind && !RAY_IS_ERR(kind) && kind->type == -RAY_SYM ? ray_sym_str(kind->i64) : NULL;
    if (ks) k = ray_str_ptr(ks);
    bool lambda = strcmp(k, "lambda") == 0;
    bool builtin = strcmp(k, "unary") == 0 || strcmp(k, "binary") == 0 || strcmp(k, "vary") == 0;
    bool color = ctx && ctx->color;

    if (color) fprintf(stdout, "\033[1;33m");
    fwrite(p, 1, len, stdout);
    if (color) fprintf(stdout, "\033[0m");
    if (builtin) {
        fprintf(stdout, ": builtin");
        if (own && !RAY_IS_ERR(own) && own->type == -RAY_SYM && own->i64 != id) {
            fputc(' ', stdout);
            print_sym(own->i64);
        }
        fprintf(stdout, " (%s)", k);
        if (sig_flag(d, "special")) fprintf(stdout, ", special form");
        if (sig_flag(d, "restricted")) fprintf(stdout, ", restricted under -U");
        fputc('\n', stdout);
        const char* u = usage && !RAY_IS_ERR(usage) && usage->type == -RAY_STR ? ray_str_ptr(usage) : "";
        size_t ul = usage && !RAY_IS_ERR(usage) && usage->type == -RAY_STR ? ray_str_len(usage) : 0;
        if (ul == 0) {
            fprintf(stdout, "  no usage entry\n");
        } else {
            /* the usage table joins a builtin's forms with ") or (" */
            if (color) fprintf(stdout, "\033[90m");
            size_t s = 0;
            for (size_t i = 0; i + 6 <= ul; i++) {
                if (memcmp(u + i, ") or (", 6) != 0) continue;
                fprintf(stdout, "  %.*s\n", (int)(i + 1 - s), u + s);
                s = i + 5;
                i += 5;
            }
            fprintf(stdout, "  %.*s\n", (int)(ul - s), u + s);
            if (color) fprintf(stdout, "\033[0m");
        }
        /* what it does, under the forms */
        if (summary && !RAY_IS_ERR(summary) && summary->type == -RAY_STR && ray_str_len(summary) > 0)
            fprintf(stdout, "  %.*s\n", (int)ray_str_len(summary), ray_str_ptr(summary));
    } else if (lambda) {
        fprintf(stdout, ": lambda\n");
        if (color) fprintf(stdout, "\033[90m");
        fprintf(stdout, "  (");
        fwrite(p, 1, len, stdout);
        if (params && !RAY_IS_ERR(params) && params->type == RAY_SYM)
            for (int64_t i = 0; i < params->len; i++) {
                fputc(' ', stdout);
                print_sym(ray_read_sym(ray_data(params), i, RAY_SYM, params->attrs));
            }
        fprintf(stdout, ")\n");
        if (color) fprintf(stdout, "\033[0m");
    } else if (strcmp(k, "namespace") == 0) {
        ray_t* ns = ray_inspect_lookup(id);
        ray_t* nk = ns && ns->type == RAY_DICT ? ray_dict_keys(ns) : NULL;
        fprintf(stdout, ": namespace of %lld names; :h ", (long long)(nk ? nk->len : 0));
        fwrite(p, 1, len, stdout);
        fprintf(stdout, ".<name> for one of them\n");
        if (ns) ray_release(ns);
    } else if (strcmp(k, "literal") == 0) {
        fprintf(stdout, ": a literal (b8)\n");
    } else if (strcmp(k, "value") == 0) {
        fprintf(stdout, ": a value, not a function\n");
    } else {
        fprintf(stdout, ": not defined\n");
    }
    ray_t* held[] = { kind, usage, summary, params, own, d };
    for (size_t i = 0; i < sizeof held / sizeof held[0]; i++)
        if (held[i]) { if (RAY_IS_ERR(held[i])) ray_error_free(held[i]); else ray_release(held[i]); }
    return NULL;
}

/* help/? — REPL-only.  With a name, what .repl.signature reports for it;
 * bare, every command's one-liner, walked from the table so it never gets
 * out of sync with what's registered. */
static ray_t* h_help(ray_t* arg, ray_syscmd_ctx_t* ctx) {
    if (!ctx || !ctx->repl) return RAY_NULL_OBJ;
    if (!arg_is_null(arg) && arg->type == -RAY_STR) {
        const char* p = ray_str_ptr(arg);
        size_t len = ray_str_len(arg);
        while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t' || p[len - 1] == '\r')) len--;
        if (len > 0) return help_name(p, len, ctx);
    }
    bool color = ctx->color;
    if (color) fprintf(stdout, "\033[1;33m");
    fprintf(stdout, ". Commands list:");
    if (color) fprintf(stdout, "\033[0m");
    fprintf(stdout, "\n");
    if (color) fprintf(stdout, "\033[90m");
    size_t n = 0;
    const ray_syscmd_t* tbl = ray_syscmd_table(&n);
    for (size_t i = 0; i < n; i++) {
        char tag[32];
        if (tbl[i].alias) snprintf(tag, sizeof(tag), ":%s/:%s", tbl[i].name, tbl[i].alias);
        else              snprintf(tag, sizeof(tag), ":%s",     tbl[i].name);
        fprintf(stdout, "  %-14s - %s\n", tag, tbl[i].help ? tbl[i].help : "");
    }
    if (color) fprintf(stdout, "\033[0m");
    fprintf(stdout, "\n");
    return NULL;
}

/* q/quit — REPL-only graceful exit. */
static ray_t* h_quit(ray_t* arg, ray_syscmd_ctx_t* ctx) {
    (void)arg; (void)ctx;
#ifdef RAY_FUZZING
    return ray_error("restricted", "quit disabled under fuzzing");
#else
    /* Defer to the standard exit path so atexit handlers run. */
    exit(0);
    return NULL;
#endif
}

/* ══════════════════════════════════════════
 * Registry
 * ══════════════════════════════════════════ */

static const ray_syscmd_t TABLE[] = {
    { "help",   "?",  h_help,   RAY_SYSCMD_REPL_ONLY,            "Display this help."                       },
    { "h",      NULL, h_help,   RAY_SYSCMD_REPL_ONLY,            "How NAME is called: :h NAME (or :help NAME)." },
    { "timeit", "t",  h_timeit, 0,                                "Toggle profiling on/off (or :t 0|1)."     },
    { "env",    NULL, h_env,    0,                                "List defined globals."                    },
    { "clear",  NULL, h_clear,  RAY_SYSCMD_REPL_ONLY,            "Clear the screen."                        },
    { "listen", NULL, h_listen, RAY_SYSCMD_RESTRICTED,           "Start IPC listener on PORT or \"HOST:PORT\"." },
    { "q",      NULL, h_quit,   RAY_SYSCMD_REPL_ONLY,            "Exit the REPL."                           },
    { "quit",   NULL, h_quit,   RAY_SYSCMD_REPL_ONLY,            "Exit the REPL."                           },
};
static const size_t TABLE_LEN = sizeof(TABLE) / sizeof(TABLE[0]);

const ray_syscmd_t* ray_syscmd_lookup(const char* name, size_t name_len) {
    if (!name || name_len == 0) return NULL;
    for (size_t i = 0; i < TABLE_LEN; i++) {
        const ray_syscmd_t* e = &TABLE[i];
        if (e->name && strlen(e->name) == name_len && memcmp(e->name, name, name_len) == 0)
            return e;
        if (e->alias && strlen(e->alias) == name_len && memcmp(e->alias, name, name_len) == 0)
            return e;
    }
    return NULL;
}

const ray_syscmd_t* ray_syscmd_table(size_t* out_count) {
    if (out_count) *out_count = TABLE_LEN;
    return TABLE;
}

/* ══════════════════════════════════════════
 * Dispatcher used by `.sys.cmd "..."` and the REPL `:` path.
 *
 * Splits the string into (command, args).  Looks up the command in
 * the registry.  If found, builds a Rayfall arg from the args slice
 * (RAY_NULL_OBJ for empty, otherwise an owned RAY_STR — the handler
 * can then re-coerce via arg_as_i64 etc.) and calls the handler.
 *
 * On miss with allow_shell=true, falls through to system() so users
 * can do `(.sys.cmd "ls -la")` as a shell escape.  With allow_shell=false
 * (REPL path), returns "domain" so a typo'd `:foo` doesn't hand the
 * shell anything by accident.
 * ══════════════════════════════════════════ */
ray_t* ray_syscmd_dispatch(const char* str, size_t len,
                           ray_syscmd_ctx_t* ctx, bool allow_shell) {
    /* Trim leading whitespace */
    size_t i = 0;
    while (i < len && (str[i] == ' ' || str[i] == '\t')) i++;
    if (i >= len) return ray_error("domain", "empty command");

    /* First word = command name (until whitespace). */
    size_t name_start = i;
    while (i < len && str[i] != ' ' && str[i] != '\t') i++;
    size_t name_len = i - name_start;

    /* Args = rest, leading whitespace trimmed. */
    while (i < len && (str[i] == ' ' || str[i] == '\t')) i++;
    const char* args_p = str + i;
    size_t args_len = len - i;

    const ray_syscmd_t* e = ray_syscmd_lookup(str + name_start, name_len);
    if (!e) {
#ifdef RAY_FUZZING
        /* No shell escape under fuzzing. */
        return ray_error("restricted", "shell disabled under fuzzing");
#endif
        if (!allow_shell)
            return ray_error("domain", "unknown command");
        /* Shell fallback — pass the entire original string verbatim
         * so quoting/redirection survives.  Match .sys.exec semantics:
         * return the host shell's exit code.
         *
         * Project policy: no libc malloc/free.  Use a stack buffer for
         * the common short-command case; fall back to the internal
         * allocator for anything larger. */
        char  stackbuf[1024];
        char* cmd;
        ray_t* cmd_block = NULL;
        if (len + 1 <= sizeof(stackbuf)) {
            cmd = stackbuf;
        } else {
            cmd_block = ray_alloc(len + 1);
            if (!cmd_block) return ray_error("oom", NULL);
            cmd = (char*)ray_data(cmd_block);
        }
        memcpy(cmd, str, len);
        cmd[len] = '\0';
        int rc = system(cmd);
        if (cmd_block) ray_free(cmd_block);
        /* the shell's exit code, as .sys.exec gives it, not the wait
         * status system() returns ("exit 3" was 768, #1740) */
        return ray_i64(ray_shell_exit_code(rc));
    }

    if (!e->fn) return ray_error("nyi", "command has no handler");

    /* REPL-only commands (clear / q / help) are reachable only when
     * a REPL context was supplied — typing them in a Rayfall script
     * via .sys.cmd would have no useful effect, so reject early with
     * a clear domain error rather than silently no-op'ing. */
    if ((e->flags & RAY_SYSCMD_REPL_ONLY) && (!ctx || !ctx->repl))
        return ray_error("domain", "command is REPL-only");

    ray_t* arg = (args_len > 0) ? ray_str(args_p, args_len) : RAY_NULL_OBJ;
    ray_t* result = e->fn(arg, ctx);
    if (arg && arg != RAY_NULL_OBJ) ray_release(arg);
    return result ? result : RAY_NULL_OBJ;
}

/* ══════════════════════════════════════════
 * Rayfall builtins:
 *   (.sys.cmd "name args")  → string-dispatched
 *   (.sys.<name> arg)       → direct, typed
 *
 * The direct builtins are registered from eval.c at startup; this
 * file just exposes the entry point each one wraps.
 * ══════════════════════════════════════════ */

ray_t* ray_syscmd_string_dispatch_fn(ray_t* x) {
    if (!ray_is_atom_local(x) || x->type != -RAY_STR)
        return ray_error("type", ".sys.cmd expects a string");
    return ray_syscmd_dispatch(ray_str_ptr(x), ray_str_len(x),
                               /*ctx=*/NULL, /*allow_shell=*/true);
}

/* Adapter for direct `.sys.<name>` invocation: pass the user's arg
 * straight to the named handler, no string parsing. */
static ray_t* invoke_by_name(const char* name, ray_t* arg) {
    const ray_syscmd_t* e = ray_syscmd_lookup(name, strlen(name));
    if (!e || !e->fn) return ray_error("nyi", NULL);
    if (e->flags & RAY_SYSCMD_REPL_ONLY)
        return ray_error("domain", "command is REPL-only");
    ray_syscmd_ctx_t ctx = { NULL, false };
    ray_t* r = e->fn(arg, &ctx);
    return r ? r : RAY_NULL_OBJ;
}

/* listen requires an arg; keep it unary. */
ray_t* ray_sys_listen_fn(ray_t* x) { return invoke_by_name("listen", x); }

/* timeit and env are usable with or without an arg ((.sys.timeit) =>
 * toggle, (.sys.env) => list).  Registering them variadic in eval.c
 * matches `.sys.gc`'s convention and avoids the arity error users
 * would otherwise hit calling `(.sys.env)` with no args. */
ray_t* ray_sys_timeit_fn(ray_t** args, int64_t n) {
    if (n > 1) return ray_error("arity", ".sys.timeit accepts at most one argument");
    return invoke_by_name("timeit", n > 0 ? args[0] : RAY_NULL_OBJ);
}
ray_t* ray_sys_env_fn(ray_t** args, int64_t n) {
    if (n > 1) return ray_error("arity", ".sys.env accepts at most one argument");
    (void)args;
    return invoke_by_name("env", n > 0 ? args[0] : RAY_NULL_OBJ);
}
