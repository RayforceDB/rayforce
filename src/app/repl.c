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

#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#elif !defined(RAY_OS_WINDOWS)
#define _GNU_SOURCE
#endif

#include "lang/format.h"
#include "app/repl.h"
#include "app/term.h"
#include "core/ipc.h"
#include "core/poll.h"
#include "core/pool.h"
#include "lang/env.h"
#include "lang/eval.h"
#include "lang/internal.h"
#include "lang/nfo.h"
#include "lang/parse.h"
#include "lang/syscmd.h"
#include "mem/heap.h"
#include "ops/ops.h"
#include "core/profile.h"
#include "table/sym.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(RAY_OS_WINDOWS)
#include <io.h>
#include <windows.h>
#define isatty _isatty
#define STDIN_FD 0
#else
#include <unistd.h>
#include <sys/ioctl.h>
#define STDIN_FD STDIN_FILENO
#endif

#if defined(__APPLE__)
#include <sys/types.h>
#include <sys/sysctl.h>
#endif

/* Forward declaration — defined in core/runtime.c */
const char* ray_error_msg(void);

/* ===== Progress bar renderer (Unicode, 1/8-step partial blocks) =====
 *
 * Used both by the push-based profiler progress (CSV loader) and the
 * new pull-based query progress callback. All output goes to stderr
 * and is in-place redraw via \r + \e[K. Call clear_progress() to wipe
 * the bar when the query/phase finishes. */

/* Left half-block cap, right half-block cap, full block, and the
 * seven partial-fill blocks 1/8 .. 7/8. UTF-8 bytes. */
static const char* const PB_FULL  = "\xe2\x96\x88"; /* █ */
static const char* const PB_PARTS[8] = {
    "",                 /* 0/8 — empty, we print space instead */
    "\xe2\x96\x8f",     /* 1/8 ▏ */
    "\xe2\x96\x8e",     /* 2/8 ▎ */
    "\xe2\x96\x8d",     /* 3/8 ▍ */
    "\xe2\x96\x8c",     /* 4/8 ▌ */
    "\xe2\x96\x8b",     /* 5/8 ▋ */
    "\xe2\x96\x8a",     /* 6/8 ▊ */
    "\xe2\x96\x89",     /* 7/8 ▉ */
};
static const char* const PB_CAP_L = "\xe2\x96\x95"; /* ▕ */
static const char* const PB_CAP_R = "\xe2\x96\x8f"; /* ▏ */

/* Terminal width, probed lazily once. Bar rendering never exceeds
 * (width-1) display columns so it cannot wrap onto a second line —
 * wrapped bars are impossible to clear reliably with a single-line
 * \e[2K, and were leaving artefacts on screen. */
static int progress_term_cols(void) {
    static int cached = 0;
    if (cached) return cached;
#if defined(RAY_OS_WINDOWS)
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    HANDLE herr = GetStdHandle(STD_ERROR_HANDLE);
    if (herr && GetConsoleScreenBufferInfo(herr, &csbi)) {
        int w = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        cached = (w > 10) ? w : 80;
    } else {
        cached = 80;
    }
#else
    struct winsize ws;
    if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 10)
        cached = ws.ws_col;
    else
        cached = 80;
#endif
    return cached;
}

static const char* fmt_bytes(int64_t bytes, char* buf, size_t bufsz) {
    if (bytes < 0) bytes = 0;
    double v = (double)bytes;
    const char* unit;
    if      (v >= 1e9) { v /= 1e9; unit = "G"; }
    else if (v >= 1e6) { v /= 1e6; unit = "M"; }
    else if (v >= 1e3) { v /= 1e3; unit = "K"; }
    else               { unit = "B"; }
    snprintf(buf, bufsz, "%.1f%s", v, unit);
    return buf;
}

static void render_progress_full(int64_t done, int64_t total,
                                   const char* op, const char* phase,
                                   double elapsed_sec,
                                   int64_t mem_used, int64_t mem_budget) {
    int cols = progress_term_cols();
    /* Reserve a chunk for labels + percent + elapsed; give the rest
     * to the bar. Minimum bar is 10 cells, maximum 40. */
    int bar_width = cols - 40;
    if (bar_width > 40) bar_width = 40;
    if (bar_width < 10) bar_width = 10;
    int sub_total = bar_width * 8;
    double pct = 0.0;
    int full = 0;
    int frac = 0;
    if (total > 0) {
        pct = (double)done / (double)total;
        if (pct > 1.0) pct = 1.0;
        int sub = (int)(pct * sub_total);
        full = sub / 8;
        frac = sub % 8;
    }

    /* Build the text tail (labels + times) into a bounded buffer so
     * we can truncate it together with the bar to fit the terminal. */
    char tail[256];
    int tp = 0;
    if (total > 0)
        tp += snprintf(tail + tp, sizeof(tail) - tp, " %3.0f%%", pct * 100.0);
    if (op && *op)
        tp += snprintf(tail + tp, sizeof(tail) - tp, " \xc2\xb7 %s", op);
    if (phase && *phase)
        tp += snprintf(tail + tp, sizeof(tail) - tp, "%s%s",
                       (op && *op) ? ": " : " \xc2\xb7 ", phase);
    if (elapsed_sec > 0.0)
        tp += snprintf(tail + tp, sizeof(tail) - tp, " \xc2\xb7 %.1fs", elapsed_sec);
    if (mem_used > 0 && mem_budget > 0) {
        char ub[16], bb[16];
        fmt_bytes(mem_used, ub, sizeof(ub));
        fmt_bytes(mem_budget, bb, sizeof(bb));
        tp += snprintf(tail + tp, sizeof(tail) - tp, " \xc2\xb7 %s/%s", ub, bb);
    }

    /* Clear the line first, then draw. Using \e[2K avoids leaving
     * stale tail text when a shorter render overwrites a longer one. */
    fprintf(stderr, "\033[2K\033[0G\033[2m%s", PB_CAP_L);
    for (int i = 0; i < bar_width; i++) {
        if (i < full)               fputs(PB_FULL, stderr);
        else if (i == full && frac) fputs(PB_PARTS[frac], stderr);
        else                        fputc(' ', stderr);
    }
    fputs(PB_CAP_R, stderr);

    /* Budget for tail: terminal width minus the bar caps (2) minus
     * bar cells, minus a safety margin of 1 to avoid landing exactly
     * on the last column (which some terminals wrap anyway). */
    int budget = cols - bar_width - 3;
    if (budget < 0) budget = 0;
    if (tp > budget) tp = budget;
    fwrite(tail, 1, (size_t)tp, stderr);

    fputs("\033[0m", stderr);
    fflush(stderr);
}

/* Profiler push API — keeps the old signature for CSV loader. */
static void render_progress(int64_t done, int64_t total, const char* label) {
    render_progress_full(done, total, label, NULL, 0.0, 0, 0);
}

static void clear_progress(void) {
    /* \e[2K clears the entire current line (not just from cursor to
     * end, which is what \e[K would do). \e[0G moves the cursor to
     * column 0. Combined, this reliably wipes whatever the last
     * render left on the current terminal row. */
    fprintf(stderr, "\033[2K\033[0G");
    fflush(stderr);
}

/* Pull-based query progress adapter — called by the core runtime
 * at sync points (between ops, radix phase boundaries, pivot phase
 * transitions). Zero work when the query finishes under min_ms. */
static void repl_query_progress_cb(const ray_progress_t* p, void* user) {
    (void)user;
    if (p->final) { clear_progress(); return; }
    render_progress_full((int64_t)p->rows_done, (int64_t)p->rows_total,
                         p->op_name, p->phase, p->elapsed_sec,
                         p->mem_used, p->mem_budget);
}

/* ===== Profiler span tree printer (reads from g_ray_profile) ===== */

static int32_t profile_print_tree(int32_t idx, int32_t indent) {
    while (idx < g_ray_profile.n) {
        ray_prof_span_t* sp = &g_ray_profile.spans[idx];

        switch (sp->type) {
        case RAY_PROF_SPAN_START: {
            for (int32_t i = 0; i < indent; i++) fprintf(stdout, "\xe2\x94\x82 ");
            fprintf(stdout, "\xe2\x95\xad %s\n", sp->msg);
            idx++;
            idx = profile_print_tree(idx, indent + 1);
            if (idx < g_ray_profile.n) {
                double ms = (double)(g_ray_profile.spans[idx].ts - sp->ts) / 1e6;
                for (int32_t i = 0; i < indent; i++) fprintf(stdout, "\xe2\x94\x82 ");
                fprintf(stdout, "\xe2\x95\xb0\xe2\x94\x80\xe2\x94\xa4 %.3f ms\n", ms);
                idx++;
            }
            break;
        }
        case RAY_PROF_SPAN_END:
            return idx;
        case RAY_PROF_SPAN_TICK: {
            double ms = 0.0;
            if (idx > 0)
                ms = (double)(sp->ts - g_ray_profile.spans[idx - 1].ts) / 1e6;
            for (int32_t i = 0; i < indent; i++) fprintf(stdout, "\xe2\x94\x82 ");
            fprintf(stdout, "\xe2\x9c\xb6  %s: %.3f ms\n", sp->msg, ms);
            idx++;
            break;
        }
        }
    }
    return idx;
}

static void profile_print(bool use_color) {
    if (!g_ray_profile.active || g_ray_profile.n == 0) return;
    if (use_color) fprintf(stdout, "\033[90m");
    profile_print_tree(0, 0);
    if (use_color) fprintf(stdout, "\033[0m");
    fflush(stdout);
}

#ifndef RAYFORCE_GIT_COMMIT
#define RAYFORCE_GIT_COMMIT "unknown"
#endif
#ifndef RAYFORCE_BUILD_DATE
#define RAYFORCE_BUILD_DATE "unknown"
#endif

static void get_cpu_name(char* buf, size_t sz) {
#if defined(__linux__)
    FILE* f = fopen("/proc/cpuinfo", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "model name", 10) == 0) {
                char* p = strchr(line, ':');
                if (p) {
                    p++;
                    while (*p == ' ') p++;
                    size_t len = strlen(p);
                    if (len > 0 && p[len - 1] == '\n') p[len - 1] = '\0';
                    snprintf(buf, sz, "%s", p);
                    fclose(f);
                    return;
                }
            }
        }
        fclose(f);
    }
    snprintf(buf, sz, "unknown");
#elif defined(__APPLE__)
    size_t len = sz;
    if (sysctlbyname("machdep.cpu.brand_string", buf, &len, NULL, 0) != 0)
        snprintf(buf, sz, "unknown");
#elif defined(RAY_OS_WINDOWS)
    snprintf(buf, sz, "unknown");
#else
    snprintf(buf, sz, "unknown");
#endif
}

static int64_t get_total_mem_mb(void) {
#if defined(__linux__)
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_sz = sysconf(_SC_PAGESIZE);
    if (pages > 0 && page_sz > 0)
        return (int64_t)pages * (int64_t)page_sz / (1024 * 1024);
    return 0;
#elif defined(__APPLE__)
    int64_t mem = 0;
    size_t len = sizeof(mem);
    sysctlbyname("hw.memsize", &mem, &len, NULL, 0);
    return mem / (1024 * 1024);
#elif defined(RAY_OS_WINDOWS)
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);
    return (int64_t)(ms.ullTotalPhys / (1024 * 1024));
#else
    return 0;
#endif
}

static void print_banner(void) {
    char cpu[256];
    get_cpu_name(cpu, sizeof(cpu));
    int64_t mem_mb = get_total_mem_mb();
    int ncores = (int)sysconf(_SC_NPROCESSORS_ONLN);

    /* "Using" count reflects the actual worker-pool size, not ncores.
     * ray_pool_get() is a lazy initializer — callers might not have
     * touched it yet, so compute the effective pool size the same way
     * the pool default does. */
    uint32_t pool_workers = 0;
    {
        ray_pool_t* p = ray_pool_get();
        if (p) pool_workers = ray_pool_total_workers(p);
    }
    if (pool_workers == 0)
        pool_workers = (uint32_t)ncores;

    fprintf(stdout,
        "\033[1m"
        "  RayforceDB: %s %s\n"
        "  %s %"PRId64"(MB) %d core(s)\n"
        "  Using %u worker(s)\n"
#ifdef DEBUG
        "  Build: debug (ASan + UBSan, -O0)\n"
#endif
        "  Documentation: https://rayforcedb.com/\n"
        "  Github: https://github.com/RayforceDB/rayforce\n"
        "\033[0m",
        ray_version_string(), RAYFORCE_BUILD_DATE,
        cpu, mem_mb, ncores,
        pool_workers);
}

#define PIPE_BUF_SIZE 4096

/* Format a rich error message with source snippet, carets, and stack trace. */
static void fmt_error_with_trace(FILE* fp, ray_t* err, ray_t* trace, bool use_color) {
    char err_code[8] = {0};
    memcpy(err_code, err->sdata, err->slen < 7 ? err->slen : 7);
    const char* detail = ray_error_msg();

    fprintf(fp, "\n");
    if (use_color) fprintf(fp, "\033[1;31m");
    fprintf(fp, "  \xc3\x97 Error: %s", err_code);
    if (use_color) fprintf(fp, "\033[0m");
    fprintf(fp, "\n");

    int64_t nframes = ray_len(trace);
    int64_t show = nframes < 5 ? nframes : 5;

    for (int64_t fi = 0; fi < show; fi++) {
        ray_t* frame = ((ray_t**)ray_data(trace))[fi];
        if (!frame || frame->type != RAY_LIST || ray_len(frame) < 4) continue;
        ray_t** felems = (ray_t**)ray_data(frame);

        ray_span_t span;
        span.id = felems[0] ? felems[0]->i64 : 0;
        if (span.id == 0) continue;

        const char* fname = "repl";
        size_t fname_len = 4;
        if (felems[1] && !RAY_IS_ERR(felems[1])) {
            fname = ray_str_ptr(felems[1]);
            fname_len = ray_str_len(felems[1]);
        }

        const char* source = "";
        size_t src_len = 0;
        if (felems[3] && !RAY_IS_ERR(felems[3])) {
            source = ray_str_ptr(felems[3]);
            src_len = ray_str_len(felems[3]);
        }

        int line_num = span.start_line;
        const char* line_start = source;
        int current_line = 0;
        while (current_line < line_num && line_start < source + src_len) {
            if (*line_start == '\n') current_line++;
            line_start++;
        }
        const char* line_end = line_start;
        while (line_end < source + src_len && *line_end != '\n') line_end++;
        int line_len = (int)(line_end - line_start);

        int display_line = line_num + 1;
        int gutter = 1;
        { int tmp = display_line; while (tmp >= 10) { gutter++; tmp /= 10; } }

        fprintf(fp, "\n");
        if (use_color) fprintf(fp, "\033[90m");
        fprintf(fp, "   \xe2\x95\xad\xe2\x94\x80[");
        if (use_color) fprintf(fp, "\033[36m");
        fprintf(fp, "%.*s", (int)fname_len, fname);
        if (use_color) fprintf(fp, "\033[33m");
        fprintf(fp, ":%d:%d", display_line, span.start_col + 1);
        if (use_color) fprintf(fp, "\033[90m");
        fprintf(fp, "]\n");

        if (use_color) fprintf(fp, "\033[36m");
        fprintf(fp, " %*d", gutter, display_line);
        if (use_color) fprintf(fp, "\033[90m");
        fprintf(fp, " \xe2\x94\x82 ");
        if (use_color) fprintf(fp, "\033[0m");
        fprintf(fp, "%.*s\n", line_len, line_start);

        if (use_color) fprintf(fp, "\033[90m");
        fprintf(fp, " %*s \xe2\x94\x82 ", gutter, "");
        for (int i = 0; i < span.start_col; i++) fputc(' ', fp);
        if (use_color) fprintf(fp, "\033[35m");
        fprintf(fp, "\xe2\x96\xb2\n");

        if (use_color) fprintf(fp, "\033[90m");
        fprintf(fp, " %*s \xe2\x94\x82 ", gutter, "");
        for (int i = 0; i < span.start_col; i++) fputc(' ', fp);
        if (use_color) fprintf(fp, "\033[35m");
        fprintf(fp, "\xe2\x95\xb0\xe2\x94\x80 ");
        if (use_color) fprintf(fp, "\033[1;31m");
        if (fi == 0) {
            fprintf(fp, "%s", err_code);
            if (detail) fprintf(fp, ": %s", detail);
        }
        if (use_color) fprintf(fp, "\033[0m");
        fprintf(fp, "\n");

        if (use_color) fprintf(fp, "\033[90m");
        fprintf(fp, "   \xe2\x95\xb0\xe2\x94\x80 in ");
        if (felems[2] && !RAY_IS_ERR(felems[2])) {
            if (use_color) fprintf(fp, "\033[32m");
            fprintf(fp, "%.*s", (int)ray_str_len(felems[2]), ray_str_ptr(felems[2]));
        } else {
            if (use_color) fprintf(fp, "\033[32m");
            fprintf(fp, "\xce\xbb");
        }
        if (use_color) fprintf(fp, "\033[0m");
        fprintf(fp, "\n");
    }

    if (nframes > show) {
        if (use_color) fprintf(fp, "\033[90m");
        fprintf(fp, "   ... %" PRId64 " more frames\n", nframes - show);
        if (use_color) fprintf(fp, "\033[0m");
    }
    fprintf(fp, "\n");
}

/* Pretty-print a result value */
static void repl_print_result(FILE* fp, ray_t* val, bool use_color) {
    if (!val || RAY_IS_NULL(val)) return;
    if (RAY_IS_ERR(val)) {
        ray_t* trace = ray_get_error_trace();
        if (trace && ray_len(trace) > 0) {
            fmt_error_with_trace(fp, val, trace, use_color);
            ray_clear_error_trace();
        } else {
            char err_code[8] = {0};
            memcpy(err_code, val->sdata, val->slen < 7 ? val->slen : 7);
            if (use_color) fprintf(fp, "\033[1;31m");
            fprintf(fp, "error: %s", err_code);
            const char* detail = ray_error_msg();
            if (detail) fprintf(fp, ": %s", detail);
            if (use_color) fprintf(fp, "\033[0m");
            fprintf(fp, "\n");
        }
        return;
    }
    ray_fmt_print(fp, val, 1);
    fprintf(fp, "\n");
}

ray_repl_t* ray_repl_create(ray_poll_t* poll) {
    ray_t* block = ray_alloc(sizeof(ray_repl_t));
    if (!block) return NULL;
    ray_repl_t* repl = (ray_repl_t*)ray_data(block);
    memset(repl, 0, sizeof(*repl));
    repl->_block = block;
    repl->poll   = poll;
    repl->id     = -1;
    /* Inherit profiler state set at startup (-t N) so the banner
     * and the first eval-and-print both see the correct flag. */
    repl->timeit = g_ray_profile.active;

    if (isatty(STDIN_FD)) {
        repl->term = ray_term_create();
        if (repl->term)
            ray_term_install_signals(repl->term);
        /* Wire the Unicode progress bar for long-running queries —
         * only on a tty so piped / scripted runs stay clean. Default
         * show-after is 500 ms (rayforce's
         * interactive feel wants the bar on sub-second queries too).
         * Override via RAY_PROGRESS_MIN_MS env var. */
        if (isatty(STDERR_FILENO)) {
            uint64_t min_ms = 500;
            const char* s = getenv("RAY_PROGRESS_MIN_MS");
            if (s && *s) {
                long v = strtol(s, NULL, 10);
                if (v >= 0) min_ms = (uint64_t)v;
            }
            ray_progress_set_callback(repl_query_progress_cb, NULL, min_ms, 100);
        }
    }
    return repl;
}

void ray_repl_destroy(ray_repl_t* repl) {
    if (!repl) return;
    if (repl->term) {
        ray_term_destroy(repl->term);
    }
    ray_free(repl->_block);
}

/* ===== Remote-REPL session ==========================================
 *
 * (.repl.connect "host:port") routes subsequent local REPL inputs to a
 * remote rayforce via ray_ipc_send_verbose() instead of ray_eval_str.
 * This is a thin per-line redirect on top of the existing .ipc.* family
 * — no new wire path, no separate event loop.  Single session at a
 * time (matches kdb+'s remote-mode UX); reconnect to a different
 * address closes the previous handle automatically.
 *
 * State lives in this file so eval_and_print can check it without
 * pulling in another header or a new compile unit. */

/* Pretty-print a ray_t to a stdio stream — defined in ops/builtins.c
 * (used by `(println val)`).  Not in a public header. */
extern void ray_lang_print(FILE* fp, ray_t* val);

static int64_t      g_remote_handle    = -1;
static char         g_remote_addr[128] = {0};
/* The currently-running interactive REPL's terminal, set by
 * ray_repl_run before entering the line loop.  Used by .repl.connect /
 * .repl.disconnect to update the prompt prefix without pushing a
 * pointer through the eval pipeline.  NULL in piped / file mode —
 * connect still works (state is updated; eval_and_print_remote uses
 * it), the prompt simply has no prefix to render. */
static ray_term_t*  g_active_term      = NULL;

bool ray_repl_remote_active(void) { return g_remote_handle >= 0; }
int64_t ray_repl_remote_handle(void) { return g_remote_handle; }
const char* ray_repl_remote_addr(void) {
    return g_remote_handle >= 0 ? g_remote_addr : NULL;
}

ray_t* ray_repl_connect_fn(ray_t* host_port_str) {
    if (!host_port_str || !ray_is_atom(host_port_str) ||
        host_port_str->type != -RAY_STR)
        return ray_error("type", "expected host:port string");

    /* .ipc.open already handles the host:port[:user:password] split
     * and the connect-error-to-rayfall-error mapping; reusing it
     * keeps .repl.connect a one-line wrapper instead of a parallel
     * implementation that drifts. */
    ray_t* opened = ray_hopen_fn(host_port_str);
    if (!opened || RAY_IS_ERR(opened)) return opened;
    if (!ray_is_atom(opened) ||
        (opened->type != -RAY_I64 && opened->type != -RAY_I32)) {
        ray_release(opened);
        return ray_error("io", "ipc.open returned non-handle");
    }

    int64_t h = (opened->type == -RAY_I64) ? opened->i64 : opened->i32;

    /* Drop any prior session before swapping the handle — leaving the
     * old socket open would leak the server-side connection slot. */
    if (g_remote_handle >= 0 && g_remote_handle != h)
        ray_ipc_close(g_remote_handle);
    g_remote_handle = h;

    size_t n = ray_str_len(host_port_str);
    if (n >= sizeof(g_remote_addr)) n = sizeof(g_remote_addr) - 1;
    memcpy(g_remote_addr, ray_str_ptr(host_port_str), n);
    g_remote_addr[n] = '\0';

    if (g_active_term) ray_term_set_prompt_prefix(g_active_term, g_remote_addr);
    return opened;
}

ray_t* ray_repl_disconnect_fn(ray_t** args, int64_t n) {
    (void)args; (void)n;
    if (g_remote_handle < 0) return RAY_NULL_OBJ;
    ray_ipc_close(g_remote_handle);
    g_remote_handle    = -1;
    g_remote_addr[0]   = '\0';
    if (g_active_term) ray_term_set_prompt_prefix(g_active_term, NULL);
    return RAY_NULL_OBJ;
}

/* eval_and_print's remote branch: send the raw input string to the
 * connected server with RAY_IPC_FLAG_VERBOSE set so server-side
 * stdout/stderr written during eval are captured and returned to us
 * inside the response list.  Print captured output first, then the
 * evaluated result (if any). */
static void eval_and_print_remote(const char* input) {
    ray_t* msg = ray_str(input, strlen(input));
    if (!msg) {
        fprintf(stderr, "error: oom building message\n");
        return;
    }

    ray_t* resp = ray_ipc_send_verbose(g_remote_handle, msg);
    ray_release(msg);

    if (!resp) {
        fprintf(stderr, "error: no response\n");
        return;
    }
    if (RAY_IS_ERR(resp)) {
        const char* what = (const char*)resp->sdata;
        fprintf(stderr, "error: %s\n", what && *what ? what : "ipc");
        /* Do NOT auto-disconnect on error — most rayfall errors (type,
         * domain, parse, etc.) are recoverable at the REPL level.  The
         * user can call (.repl.disconnect) explicitly if they prefer. */
        return;
    }

    /* eval_payload's VERBOSE path produces [captured_str, result].
     * If the server fell back to the no-capture path (tmpfile/dup
     * failure on its side) it still wraps as a list with empty
     * captured_str — clients can rely on the shape. */
    ray_t* result = resp;
    if (resp->type == RAY_LIST && resp->len >= 2) {
        ray_t** items = (ray_t**)ray_data(resp);
        ray_t*  cap   = items[0];
        if (cap && cap->type == -RAY_STR) {
            size_t cl = ray_str_len(cap);
            if (cl > 0) fwrite(ray_str_ptr(cap), 1, cl, stdout);
        }
        result = items[1];
    }
    if (result && result != RAY_NULL_OBJ && !RAY_IS_ERR(result)) {
        ray_lang_print(stdout, result);
        fputc('\n', stdout);
    } else if (result && RAY_IS_ERR(result)) {
        fprintf(stderr, "error: %s\n", (const char*)result->sdata);
    }
    fflush(stdout);
    ray_release(resp);
}

static void eval_and_print(ray_term_t* term, const char* input,
                           bool use_color, bool timeit) {
    /* Remote-REPL redirect: when a session is active every input goes
     * to the connected server.  Profiling/timing in remote mode is the
     * server's responsibility (server-side -t setting), so we skip the
     * local profiler scaffolding entirely and never touch ray_eval.
     *
     * Exception: `.repl.*` calls always evaluate locally — they manage
     * the client-side session state, so forwarding them to the server
     * would be a no-op (server's own session state is irrelevant) and
     * would prevent the user from ever calling .repl.disconnect from
     * inside a remote session. */
    if (g_remote_handle >= 0) {
        const char* p = input;
        while (*p == ' ' || *p == '\t') p++;
        bool is_repl_ctl = (strncmp(p, "(.repl.", 7) == 0);
        if (!is_repl_ctl) {
            eval_and_print_remote(input);
            return;
        }
    }

    bool profiling = timeit && g_ray_profile.active;

    if (profiling) {
        ray_profile_reset();
        g_ray_profile.progress_cb = render_progress;
        ray_profile_span_start("top-level");
    }

    ray_term_clear_interrupt();
    ray_eval_clear_interrupt();
    if (term) ray_term_eval_begin(term);

    ray_t* nfo = ray_nfo_create("repl", 4, input, strlen(input));
    ray_clear_error_trace();

    ray_t* parsed = ray_parse_with_nfo(input, nfo);
    if (profiling) ray_profile_tick("parse");

    ray_t* result;
    if (RAY_IS_ERR(parsed)) {
        result = parsed;
    } else {
        ray_t* prev_nfo = ray_eval_get_nfo();
        ray_eval_set_nfo(nfo);
        result = ray_eval(parsed);
        ray_eval_set_nfo(prev_nfo);
        if (profiling) ray_profile_tick("eval");
        ray_release(parsed);
    }
    ray_release(nfo);

    if (term) ray_term_eval_end(term);

    /* Clear any pull-based progress state left over from this
     * top-level eval. Some paths (e.g. ray_group_fn invoked from
     * the select builtin) emit progress updates without going
     * through ray_execute, so the ray_execute wrapper's cleanup
     * never runs and the bar would otherwise stay on screen. This
     * is the one guaranteed per-eval boundary in the REPL. */
    ray_progress_end();

    if (ray_is_lazy(result)) {
        result = ray_lazy_materialize(result);
        if (profiling) ray_profile_tick("materialize");
    }

    if (profiling) {
        ray_profile_span_end("top-level");
        if (g_ray_profile.progress_total > 0) clear_progress();
        g_ray_profile.progress_cb = NULL;
    }

    if (ray_term_interrupted()) {
        ray_term_clear_interrupt();
        ray_eval_clear_interrupt();
        fprintf(stdout, "\n^C\n");
        fflush(stdout);
        if (result && !RAY_IS_ERR(result)) ray_release(result);
        return;
    }

    if (result) {
        repl_print_result(stdout, result, use_color);
        fflush(stdout);
        if (!RAY_IS_ERR(result)) ray_release(result);
    }

    if (profiling) profile_print(use_color);
}

/* `type_label` and `cmd_match` were inlined into the previous bespoke
 * `:t`/`:env`/`:clear` dispatcher.  The shared lang/syscmd registry
 * subsumes both; their replacements live in lang/syscmd.c. */

static bool handle_command(ray_repl_t* repl, const char* str, size_t len) {
    if (len == 0 || str[0] != ':') return false;

    bool color = (repl->term != NULL);

    /* Strip the leading `:`; the rest is "name args" — same shape the
     * `.sys.cmd` builtin parses.  Route through the shared registry so
     * REPL commands and Rayfall-level system commands stay in lockstep
     * (any new entry in lang/syscmd.c shows up here automatically). */
    ray_syscmd_ctx_t ctx = { repl, color };
    ray_t* result = ray_syscmd_dispatch(str + 1, len - 1, &ctx, /*allow_shell=*/false);

    if (result && RAY_IS_ERR(result)) {
        ray_err_t kind = ray_err_from_obj(result);
        ray_release(result);
        if (kind == RAY_ERR_DOMAIN) {
            /* Unknown command — keep the v1-style hint so muscle memory
             * for `:?` and `:t` still works on typos. */
            if (color) fprintf(stdout, "\033[1;33m");
            fprintf(stdout, ". Unknown command: %.*s.", (int)len, str);
            if (color) fprintf(stdout, "\033[0m");
            fprintf(stdout, "\n");
            if (color) fprintf(stdout, "\033[90m");
            fprintf(stdout, "Type :? for help.");
            if (color) fprintf(stdout, "\033[0m");
            fprintf(stdout, "\n");
        } else {
            /* Real error from the handler — surface its message. */
            if (color) fprintf(stdout, "\033[1;31m");
            fprintf(stdout, ". error\n");
            if (color) fprintf(stdout, "\033[0m");
        }
    } else if (result && result != RAY_NULL_OBJ) {
        /* Handler returned a value (e.g. listener id) — discard; the
         * REPL surface for system commands is "do the thing", not
         * "print the return value".  Real Rayfall expressions go
         * through the regular eval path. */
        ray_release(result);
    }

    /* Track timeit echoing so the prompt reflects current state. */
    repl->timeit = g_ray_profile.active;
    return true;
}

/* ===== Interactive mode — poll-driven ===== */

/* repl_read callback: called when stdin has data.
 * Reads one byte via term_getc, feeds it to term_feed.
 * Returns a ray_t* string when a line is complete, NULL otherwise. */
static ray_t* repl_read(ray_poll_t* poll, ray_selector_t* sel)
{
    (void)poll;
    ray_repl_t* repl = (ray_repl_t*)sel->data;
    ray_term_t* term = repl->term;

    int64_t sz = ray_term_getc(term);
    if (sz <= 0) {
        if (sz == -2) {
            /* SIGINT — clear line and re-prompt */
            ray_term_clear_interrupt();
            ray_eval_clear_interrupt();
            term->comp_cycling = 0;
            term->esc_state = 0;
            term->buf_len = 0;
            term->buf_pos = 0;
            term->multiline_len = 0;
#if !defined(RAY_OS_WINDOWS)
            { ssize_t r_ = write(STDOUT_FILENO, "^C\n", 3); (void)r_; }
#endif
            ray_term_prompt(term);
            fflush(stdout);
            return NULL;
        }
        /* EOF (Ctrl-D).  Inside a remote session, treat as
         * "disconnect from remote, return to local prompt" rather
         * than killing the whole REPL — matches ssh / mosh UX where
         * the first Ctrl-D logs out of the remote and the second
         * exits the local shell. */
        if (ray_repl_remote_active()) {
            ray_t* args = NULL;
            ray_release(ray_repl_disconnect_fn(&args, 0));
            term->buf_len = 0;
            term->buf_pos = 0;
            term->multiline_len = 0;
            ray_term_prompt(term);
            fflush(stdout);
            return NULL;
        }
        ray_poll_exit(poll, 0);
        return NULL;
    }

    ray_t* line = ray_term_feed(term);
    if (line == RAY_TERM_EOF) {
        if (ray_repl_remote_active()) {
            ray_t* args = NULL;
            ray_release(ray_repl_disconnect_fn(&args, 0));
            term->buf_len = 0;
            term->buf_pos = 0;
            term->multiline_len = 0;
            ray_term_prompt(term);
            fflush(stdout);
            return NULL;
        }
        ray_poll_exit(poll, 0);
        return NULL;
    }
    return line;
}

/* repl_on_data callback: receives a complete line string.
 * Handles commands, evals, and prints. */
static ray_t* repl_on_data(ray_poll_t* poll, ray_selector_t* sel, void* data)
{
    (void)poll;
    ray_repl_t* repl = (ray_repl_t*)sel->data;
    ray_t* line = (ray_t*)data;

    const char* str = ray_str_ptr(line);
    size_t len = ray_str_len(line);

    if (len == 0) {
        ray_release(line);
        ray_term_begin(repl->term);
        return NULL;
    }

    /* Exit commands */
    if ((len == 2 && memcmp(str, "\\\\", 2) == 0) ||
        (len == 4 && memcmp(str, "exit", 4) == 0)) {
        ray_release(line);
        ray_poll_exit(poll, 0);
        return NULL;
    }

    /* REPL commands starting with ':' */
    if (str[0] == ':') {
        size_t clen = len - 1;
        const char* cmd = str + 1;
        if ((clen == 1 && cmd[0] == 'q') ||
            (clen == 4 && memcmp(cmd, "quit", 4) == 0)) {
            ray_release(line);
            ray_poll_exit(poll, 0);
            return NULL;
        }
        handle_command(repl, str, len);
        ray_release(line);
        ray_term_begin(repl->term);
        return NULL;
    }

    eval_and_print(repl->term, str, true, repl->timeit);
    ray_release(line);
    ray_term_begin(repl->term);
    return NULL;
}

static void run_interactive(ray_repl_t* repl) {
    ray_term_t* term = repl->term;
    print_banner();

    if (repl->poll) {
        /* Register stdin in poll */
        ray_poll_reg_t reg = {0};
        reg.fd       = STDIN_FD;
        reg.type     = RAY_SEL_STDIN;
        reg.read_fn  = repl_read;
        reg.data_fn  = repl_on_data;
        reg.data     = repl;

        repl->id = ray_poll_register(repl->poll, &reg);

        ray_term_begin(term);
        ray_poll_run(repl->poll);
    } else {
        /* Fallback: no poll, run blocking loop */
        ray_term_begin(term);
        for (;;) {
            int64_t sz = ray_term_getc(term);
            if (sz <= 0) {
                if (sz == -2) {
                    ray_term_clear_interrupt();
                    ray_eval_clear_interrupt();
                    term->comp_cycling = 0;
                    term->esc_state = 0;
                    term->buf_len = 0;
                    term->buf_pos = 0;
                    term->multiline_len = 0;
#if !defined(RAY_OS_WINDOWS)
                    { ssize_t r_ = write(STDOUT_FILENO, "^C\n", 3); (void)r_; }
#endif
                    ray_term_prompt(term);
                    fflush(stdout);
                    continue;
                }
                break;
            }
            ray_t* line = ray_term_feed(term);
            if (line == RAY_TERM_EOF) break;
            if (!line) continue;

            const char* str = ray_str_ptr(line);
            size_t len = ray_str_len(line);

            if (len == 0) {
                ray_release(line);
                ray_term_begin(term);
                continue;
            }

            if ((len == 2 && memcmp(str, "\\\\", 2) == 0) ||
                (len == 4 && memcmp(str, "exit", 4) == 0)) {
                ray_release(line);
                break;
            }

            if (str[0] == ':') {
                size_t clen = len - 1;
                const char* cmd = str + 1;
                if ((clen == 1 && cmd[0] == 'q') ||
                    (clen == 4 && memcmp(cmd, "quit", 4) == 0)) {
                    ray_release(line);
                    break;
                }
                handle_command(repl, str, len);
                ray_release(line);
                ray_term_begin(term);
                continue;
            }

            eval_and_print(repl->term, str, true, repl->timeit);
            ray_release(line);
            ray_term_begin(term);
        }
    }
}

/* ===== Piped mode ===== */

/* Parse state for bracket_delta, preserved across chunk boundaries. */
typedef struct {
    int in_string;
    int in_comment;
} bracket_state_t;

static int32_t bracket_delta_s(const char* s, size_t len,
                               bracket_state_t* state) {
    int32_t depth = 0;
    int in_string  = state ? state->in_string  : 0;
    int in_comment = state ? state->in_comment : 0;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (in_comment) {
            if (c == '\n') in_comment = 0;
            continue;
        }
        if (in_string) {
            if (c == '\\' && i + 1 < len) { i++; continue; }
            if (c == '"') in_string = 0;
            continue;
        }
        if (c == '"') { in_string = 1; continue; }
        if (c == ';') {
            in_comment = 1;
            continue;
        }
        if (c == '(' || c == '[' || c == '{') depth++;
        else if (c == ')' || c == ']' || c == '}') depth--;
    }
    if (state) {
        state->in_string  = in_string;
        state->in_comment = in_comment;
    }
    return depth;
}

static int32_t bracket_delta(const char* s, size_t len) {
    return bracket_delta_s(s, len, NULL);
}

static int32_t count_unmatched(const char* s, size_t len) {
    int32_t d = bracket_delta(s, len);
    return d > 0 ? d : 0;
}

static void run_piped(ray_repl_t* repl) {
    char line[PIPE_BUF_SIZE];
    char accum[PIPE_BUF_SIZE];
    size_t accum_len = 0;
    bool mid_line = false;

    while (fgets(line, PIPE_BUF_SIZE, stdin)) {
        size_t len = strlen(line);
        bool had_newline = (len > 0 && line[len - 1] == '\n');
        if (had_newline) line[--len] = '\0';

        /* Skip empty lines when no accumulation in progress */
        if (len == 0 && accum_len == 0 && !mid_line)
            continue;

        if (accum_len == 0 && !mid_line) {
            if (strcmp(line, "\\\\") == 0 || strcmp(line, "exit") == 0) break;

            if (line[0] == ':') {
                size_t clen = len - 1;
                const char* cmd = line + 1;
                if ((clen == 1 && cmd[0] == 'q') ||
                    (clen == 4 && memcmp(cmd, "quit", 4) == 0))
                    break;
                handle_command(repl, line, len);
                continue;
            }
        }

        /* Append chunk to accumulator */
        size_t needed = accum_len + len + (accum_len > 0 && !mid_line ? 1 : 0);
        if (needed < PIPE_BUF_SIZE) {
            if (accum_len > 0 && !mid_line) accum[accum_len++] = '\n';
            memcpy(accum + accum_len, line, len);
            accum_len += len;
            accum[accum_len] = '\0';
        } else {
            fprintf(stderr, "error: input too large (max %d bytes)\n",
                    PIPE_BUF_SIZE - 1);
            bracket_state_t bs = {0, 0};
            int32_t depth = bracket_delta_s(accum, accum_len, &bs);
            if (accum_len > 0 && !mid_line) bs.in_comment = 0;
            depth += bracket_delta_s(line, len, &bs);
            accum_len = 0;
            mid_line = false;
            while (!had_newline) {
                if (!fgets(line, PIPE_BUF_SIZE, stdin)) break;
                len = strlen(line);
                had_newline = (len > 0 && line[len - 1] == '\n');
                if (had_newline) {
                    line[--len] = '\0';
                    bs.in_comment = 0;
                }
                depth += bracket_delta_s(line, len, &bs);
            }
            while (depth > 0) {
                if (!fgets(line, PIPE_BUF_SIZE, stdin)) break;
                len = strlen(line);
                had_newline = (len > 0 && line[len - 1] == '\n');
                if (had_newline) {
                    line[--len] = '\0';
                    bs.in_comment = 0;
                }
                depth += bracket_delta_s(line, len, &bs);
                while (!had_newline) {
                    if (!fgets(line, PIPE_BUF_SIZE, stdin)) break;
                    len = strlen(line);
                    had_newline = (len > 0 && line[len - 1] == '\n');
                    if (had_newline) {
                        line[--len] = '\0';
                        bs.in_comment = 0;
                    }
                    depth += bracket_delta_s(line, len, &bs);
                }
            }
            continue;
        }

        if (!had_newline) {
            mid_line = true;
            continue;
        }
        mid_line = false;

        if (count_unmatched(accum, accum_len) == 0) {
            eval_and_print(NULL, accum, false, repl->timeit);
            accum_len = 0;
        }
    }

    if (accum_len > 0) {
        accum[accum_len] = '\0';
        eval_and_print(NULL, accum, false, repl->timeit);
    }

    /* If poll has registered selectors (IPC server), enter poll_run after stdin */
    if (repl->poll && repl->poll->n_sels > 0)
        ray_poll_run(repl->poll);
}

void ray_repl_run(ray_repl_t* repl) {
    /* Publish the active term so .repl.connect / .repl.disconnect can
     * adjust the prompt prefix without threading it through eval. */
    g_active_term = repl->term;
    if (repl->term) {
        run_interactive(repl);
    } else {
        run_piped(repl);
    }
    g_active_term = NULL;
}

int ray_repl_run_file(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s'\n", path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long flen = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (flen < 0) {
        fclose(f);
        fprintf(stderr, "error: cannot determine size of '%s'\n", path);
        return 1;
    }

    ray_t* block = ray_alloc((int64_t)flen + 1);
    if (!block) {
        fclose(f);
        fprintf(stderr, "error: out of memory\n");
        return 1;
    }
    char* buf = (char*)ray_data(block);
    size_t nread = fread(buf, 1, (size_t)flen, f);
    fclose(f);
    buf[nread] = '\0';

    if (nread == 0) { ray_release(block); return 0; }

    /* File mode matches eval_and_print's structure so -t 1 / :t 1
     * produces the same span layout (parse / eval / materialize) as
     * REPL input, not one opaque "eval" tick.  Colors are keyed per
     * stream: stdout (result + profile tree) and stderr (errors)
     * may be redirected independently, e.g. `rayforce -t 1 f.rfl 2>log`. */
    bool color_out = isatty(fileno(stdout));
    bool color_err = isatty(fileno(stderr));
    bool profiling = g_ray_profile.active;

    if (profiling) {
        ray_profile_reset();
        ray_profile_span_start("top-level");
    }

    ray_t* nfo = ray_nfo_create(path, strlen(path), buf, nread);
    ray_clear_error_trace();

    ray_t* parsed = ray_parse_with_nfo(buf, nfo);
    if (profiling) ray_profile_tick("parse");

    ray_t* result;
    if (RAY_IS_ERR(parsed)) {
        result = parsed;
    } else {
        ray_t* prev_nfo = ray_eval_get_nfo();
        ray_eval_set_nfo(nfo);
        result = ray_eval(parsed);
        ray_eval_set_nfo(prev_nfo);
        if (profiling) ray_profile_tick("eval");
        ray_release(parsed);
    }
    ray_release(nfo);
    ray_release(block);

    ray_progress_end();

    if (ray_is_lazy(result)) {
        result = ray_lazy_materialize(result);
        if (profiling) ray_profile_tick("materialize");
    }

    if (profiling) ray_profile_span_end("top-level");

    int rc;
    if (RAY_IS_ERR(result)) {
        repl_print_result(stderr, result, color_err);
        rc = 1;
    } else {
        if (result) {
            repl_print_result(stdout, result, color_out);
            ray_release(result);
        }
        rc = 0;
    }

    /* profile tree goes to stdout via profile_print; honour stdout's tty. */
    if (profiling) profile_print(color_out);
    return rc;
}
