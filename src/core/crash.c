/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   Fatal-signal crash handler — see crash.h.
 *
 *   Everything the handler does is async-signal-safe: fixed-string
 *   write(2), a hand-rolled hex/dec formatter (never snprintf), and
 *   backtrace_symbols_fd (which writes directly and does not malloc,
 *   unlike backtrace_symbols).  The version banner is formatted once at
 *   install time into a static buffer, and the unwinder is warmed up then
 *   too, so the handler path allocates nothing.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "core/crash.h"

#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <execinfo.h>
#endif

/* ── async-signal-safe output helpers ─────────────────────────────── */

static void cw(const char* s) {
    if (!s) return;
    ssize_t n = (ssize_t)strlen(s);
    ssize_t off = 0;
    while (off < n) {
        ssize_t w = write(STDERR_FILENO, s + off, (size_t)(n - off));
        if (w <= 0) break;
        off += w;
    }
}

/* Write an unsigned value as 0x-prefixed hex.  No libc formatting.
 * Routes through cw() so the write return value is handled (gcc's
 * warn_unused_result on write() is not silenced by a (void) cast). */
static void cw_hex(uint64_t v) {
    char buf[2 + 16 + 1];
    static const char hexd[] = "0123456789abcdef";
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++)
        buf[2 + i] = hexd[(v >> ((15 - i) * 4)) & 0xf];
    buf[2 + 16] = '\0';
    cw(buf);
}

#if !defined(_WIN32)
/* Write a small non-negative integer as decimal (backtrace frame count). */
static void cw_int(int v) {
    if (v < 0) { cw("-"); v = -v; }
    char buf[16];
    int i = (int)sizeof(buf);
    buf[--i] = '\0';
    if (v == 0) buf[--i] = '0';
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    cw(&buf[i]);
}
#endif

/* Banner precomputed at install time so the handler doesn't format it. */
static char g_banner[128];

static void crash_banner_init(void) {
    const char* v =
#ifdef RAYFORCE_VERSION
        "rayforce " RAYFORCE_VERSION
#else
        "rayforce"
#endif
#ifdef RAYFORCE_GIT_COMMIT
        " (" RAYFORCE_GIT_COMMIT ")"
#endif
        "\n";
    size_t vl = strlen(v);
    if (vl >= sizeof(g_banner)) vl = sizeof(g_banner) - 1;
    memcpy(g_banner, v, vl);
    g_banner[vl] = '\0';
}

#if defined(_WIN32)

/* ── Windows: structured exceptions ───────────────────────────────── */

static const char* exc_name(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:      return "EXCEPTION_ACCESS_VIOLATION";
        case EXCEPTION_STACK_OVERFLOW:        return "EXCEPTION_STACK_OVERFLOW";
        case EXCEPTION_ILLEGAL_INSTRUCTION:   return "EXCEPTION_ILLEGAL_INSTRUCTION";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "EXCEPTION_INT_DIVIDE_BY_ZERO";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_IN_PAGE_ERROR:         return "EXCEPTION_IN_PAGE_ERROR";
        default:                              return "exception";
    }
}

/* Top-level filter: runs only for exceptions nobody else handled.  Report
 * and let the default handling (WER / exit with the exception code) run,
 * which keeps the process exit status meaningful to the orchestrator. */
static LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* er = ep ? ep->ExceptionRecord : NULL;
    cw("\n=== rayforce fatal ");
    cw(er ? exc_name(er->ExceptionCode) : "exception");
    if (er) {
        cw(" code "); cw_hex((uint64_t)er->ExceptionCode);
        cw(" at "); cw_hex((uint64_t)(uintptr_t)er->ExceptionAddress);
        if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
            er->NumberParameters >= 2) {
            cw(" fault addr ");
            cw_hex((uint64_t)er->ExceptionInformation[1]);
        }
    }
    cw(" ===\n");
    cw(g_banner);
    return EXCEPTION_CONTINUE_SEARCH;
}

void ray_crash_install(void) {
    crash_banner_init();
    /* Reserve stack for the filter itself, so a stack-overflow exception
     * can still be reported (the Windows analogue of sigaltstack). */
    ULONG guarantee = 64 * 1024;
    (void)SetThreadStackGuarantee(&guarantee);
    SetUnhandledExceptionFilter(crash_filter);
}

#else /* POSIX */

static const char* sig_name(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGBUS:  return "SIGBUS";
        case SIGILL:  return "SIGILL";
        case SIGFPE:  return "SIGFPE";
        case SIGABRT: return "SIGABRT";
        default:      return "signal";
    }
}

/* ── the handler ──────────────────────────────────────────────────── */

static void crash_handler(int sig, siginfo_t* info, void* ucontext) {
    (void)ucontext;

    cw("\n=== rayforce fatal ");
    cw(sig_name(sig));
    if (info) { cw(" at fault addr "); cw_hex((uint64_t)(uintptr_t)info->si_addr); }
    cw(" ===\n");
    cw(g_banner);

    void* frames[64];
    int n = backtrace(frames, 64);
    /* backtrace_symbols_fd writes directly to the fd without allocating. */
    backtrace_symbols_fd(frames, n, STDERR_FILENO);
    cw("=== end backtrace (");
    cw_int(n);
    cw(" frames) ===\n");

    /* Restore the default disposition and re-raise, so the process dies
     * from the original signal: this preserves the core dump and reports
     * the correct terminating-signal exit status to the orchestrator. */
    signal(sig, SIG_DFL);
    raise(sig);
}

/* ── installation ─────────────────────────────────────────────────── */

/* Alternate signal stack, so a SIGSEGV caused by stack overflow (when the
 * normal stack is exhausted) still has room to run the handler.  Fixed
 * 64 KiB: SIGSTKSZ is a runtime sysconf() value on modern glibc (not a
 * compile-time constant), and 64 KiB comfortably exceeds it while leaving
 * ample room for the backtrace path. */
#define RAY_CRASH_ALTSTACK_SZ (64 * 1024)
static char g_altstack[RAY_CRASH_ALTSTACK_SZ];

void ray_crash_install(void) {
    /* Precompute the version banner once (async-signal-safe reuse). */
    crash_banner_init();

    /* Warm up the unwinder: the first backtrace() may dlopen libgcc and
     * allocate, which must not happen inside the handler. */
    {
        void* dummy[4];
        (void)backtrace(dummy, 4);
    }

    stack_t ss;
    ss.ss_sp    = g_altstack;
    ss.ss_size  = sizeof(g_altstack);
    ss.ss_flags = 0;
    (void)sigaltstack(&ss, NULL);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;

    static const int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
        (void)sigaction(sigs[i], &sa, NULL);
}

#endif /* _WIN32 */
