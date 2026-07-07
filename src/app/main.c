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

#define _POSIX_C_SOURCE 200809L
#include "core/poll.h"
#include "core/ipc.h"
#include "core/pool.h"
#include "core/profile.h"
#include "core/qlog.h"
#include "core/crash.h"
#include "app/repl.h"
#include "core/runtime.h"
#include "store/journal.h"
#include "ops/internal.h"
#include "ops/idxop.h"
#include "mem/heap.h"
#include <rayforce.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>

/* Parse a byte size with an optional K/M/G/T suffix (base-1024).  Returns
 * the byte count, or -1 on a malformed value.  Used by the -m/--mem RAM
 * budget flag: allocations that would push the anonymous (RAM-resident)
 * footprint past this bound spill to a disk-backed mapping instead of
 * OOM-ing (see ray_heap_set_anon_watermark). */
static int64_t parse_size_arg(const char* s) {
    if (!s || !*s) return -1;
    char* end = NULL;
    errno = 0;
    double v = strtod(s, &end);
    if (errno != 0 || end == s || v < 0) return -1;
    int64_t mult = 1;
    if (*end) {
        switch (*end) {
            case 'k': case 'K': mult = 1LL << 10; break;
            case 'm': case 'M': mult = 1LL << 20; break;
            case 'g': case 'G': mult = 1LL << 30; break;
            case 't': case 'T': mult = 1LL << 40; break;
            default: return -1;
        }
        if (end[1] != '\0') return -1;   /* trailing junk after the suffix */
    }
    return (int64_t)(v * (double)mult);
}

int main(int argc, char** argv) {
    /* Install the fatal-signal handler first: a crash during startup
     * should still produce a symbolized backtrace.  Handles the fault
     * signals only; term.c owns the graceful-shutdown set. */
    ray_crash_install();
    ray_expr_stats_init();
    ray_idx_stats_init();
    ray_runtime_t* rt = ray_runtime_create(argc, argv);
    if (!rt) { fprintf(stderr, "failed to create runtime\n"); return 1; }

    int rc = 0;
    int interactive = 0;
    const char* file = NULL;
    uint16_t port = 0;
    int n_cores = -1;      /* -1 = leave pool at lazy ncpu-1 default */
    int timeit_init = 0;   /* -t N: enable profiler at startup */
    int qlog_init = 0;     /* -Q N: enable query-statistics logging at startup */
    const char* auth_pw = NULL;
    bool auth_restricted = false;
    const char* log_base = NULL;
    ray_journal_mode_t log_mode = RAY_JOURNAL_OFF;

    /* Parse args. Supported flags:
     *   -f FILE          run script file
     *   -p PORT          IPC listen port
     *   -c N             worker-pool size (0 = auto: ncpu - 1)
     *   -t N             enable timeit at startup (N != 0 turns on)
     *   -Q N             enable query-statistics logging (N != 0 turns on)
     *   -i               interactive
     * Plus rayforce-specific:
     *   -u PW / -U PW    auth password (plain / restricted)
     *   -h               help
     */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--interactive") == 0)
            interactive = 1;
        else if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) && i + 1 < argc)
            port = (uint16_t)atoi(argv[++i]);
        else if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--cores") == 0) && i + 1 < argc) {
            int v = atoi(argv[++i]);
            if (v < 0) v = 0;
            n_cores = v;
        }
        else if ((strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--timeit") == 0) && i + 1 < argc) {
            int v = atoi(argv[++i]);
            timeit_init = (v != 0);
        }
        else if ((strcmp(argv[i], "-Q") == 0 || strcmp(argv[i], "--querylog") == 0) && i + 1 < argc) {
            int v = atoi(argv[++i]);
            qlog_init = (v != 0);
        }
        else if ((strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--file") == 0) && i + 1 < argc) {
            file = argv[++i];
        }
        else if (strcmp(argv[i], "-u") == 0 && i + 1 < argc) {
            auth_pw = argv[++i];
            auth_restricted = false;
        }
        else if (strcmp(argv[i], "-U") == 0 && i + 1 < argc) {
            auth_pw = argv[++i];
            auth_restricted = true;
        }
        else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            log_base = argv[++i];
            log_mode = RAY_JOURNAL_ASYNC;
        }
        else if (strcmp(argv[i], "-L") == 0 && i + 1 < argc) {
            log_base = argv[++i];
            log_mode = RAY_JOURNAL_SYNC;
        }
        else if ((strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--mem") == 0) && i + 1 < argc) {
            int64_t wm = parse_size_arg(argv[++i]);
            if (wm <= 0) {
                fprintf(stderr, "error: -m/--mem expects a size like 4G, 512M, or a byte count\n");
                ray_runtime_destroy(rt);
                return 1;
            }
            ray_heap_set_anon_watermark(wm);
        }
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fprintf(stdout,
                "usage: %s [-f file] [-p port] [-c cores] [-t 0|1] [-i]"
                " [-u PW | -U PW] [-l BASE | -L BASE] [-m SIZE] [file.rfl] [-- app args]\n"
                "  -f, --file FILE     run script file (or pass as a positional arg)\n"
                "  -p, --port PORT     listen for IPC clients on PORT\n"
                "  -c, --cores N       worker-pool size (0 = auto: ncpu - 1, default)\n"
                "  -t, --timeit N      enable profiler at startup (N != 0)\n"
                "  -i, --interactive   start the REPL even after running a file\n"
                "  -u PW               set plain auth password\n"
                "  -U PW               set restricted auth password\n"
                "  -l BASE             enable journal logging (BASE.log + BASE.qdb)\n"
                "  -L BASE             enable journal logging with fsync per write\n"
                "  -m, --mem SIZE      RAM budget (e.g. 4G, 512M); allocations past it\n"
                "                      spill to disk instead of OOM (default: physical RAM)\n"
                "  -h, --help          show this message\n"
                "  --                  pass following args to the app; read\n"
                "                      them in Rayfall via (.sys.args)\n"
                "\nFor a remote REPL, start rayforce locally and call\n"
                "(.repl.connect \"host:port\") inside the REPL.\n"
                "\nRunning as a service:\n"
                "  When stdin and stdout are both not a terminal (systemd\n"
                "  Type=simple, Docker, nohup &, etc.), rayforce skips the\n"
                "  REPL and runs only the IPC poll loop.  Redirect stdout\n"
                "  and stderr to a file/journal — anything (println ...)\n"
                "  prints from the server side goes there.  Example:\n"
                "    nohup rayforce -p 5000 > rayforce.log 2>&1 &\n"
                "    systemd:  StandardOutput=journal StandardError=journal\n",
                argv[0]);
            ray_runtime_destroy(rt);
            return 0;
        }
        else if (strcmp(argv[i], "--") == 0)
            break;   /* stop flag parsing; remaining tokens are app args */
        else
            file = argv[i];
    }

    /* Expose the full command line to Rayfall via (.sys.args). */
    ray_runtime_set_sys_args(ray_build_sys_args(argc, argv));

    /* Initialise the worker pool before anything else that might use it
     * (file load, REPL eval, builtins).  If -c wasn't given, leave the
     * pool to its lazy default on first use. */
    if (n_cores >= 0) {
        ray_err_t err = ray_pool_init((uint32_t)n_cores);
        if (err != RAY_OK)
            fprintf(stderr, "warning: ray_pool_init(%d) failed (%d)\n",
                    n_cores, (int)err);
    }

    /* -t at startup flips the global profiler flag. The REPL reads
     * g_ray_profile.active into repl->timeit on create. */
    if (timeit_init)
        g_ray_profile.active = true;

    /* -Q at startup arms the ambient query-statistics ring (.sys.querylog). */
    if (qlog_init)
        ray_qlog_set_enabled(true);

    ray_poll_t* poll = ray_poll_create();
    /* Publish the main poll into the runtime so runtime-level builtins
     * (`.sys.cmd "listen N"`, `.sys.listen N`) can attach listeners to
     * the same event loop the REPL/server is driving. */
    if (poll) ray_runtime_set_poll(poll);

    if (poll && auth_pw) {
        size_t pw_len = strlen(auth_pw);
        if (pw_len >= sizeof(poll->auth_secret))
            pw_len = sizeof(poll->auth_secret) - 1;
        memcpy(poll->auth_secret, auth_pw, pw_len);
        poll->auth_secret[pw_len] = '\0';
        poll->restricted = auth_restricted;
    }

    /* Open journal BEFORE the IPC listener accepts any connection.
     * Replay must complete with a stable global env, and any badtail
     * is fatal — exit non-zero so a supervisor can flag the failure
     * rather than silently coming up with a partially-applied state. */
    if (log_base) {
        ray_err_t le = ray_journal_open(log_base, log_mode);
        if (le != RAY_OK) {
            fprintf(stderr, "log: open failed for base=%s (rc=%d)\n",
                    log_base, (int)le);
            if (poll) ray_poll_destroy(poll);
            ray_runtime_destroy(rt);
            return 2;
        }
    }

    /* Start IPC server if port specified */
    if (port > 0) {
        if (poll && ray_ipc_listen(poll, port) >= 0)
            fprintf(stderr, "listening on port %u\n", port);
        else
            fprintf(stderr, "failed to listen on port %u: %s\n",
                    port, strerror(errno));
    }

    /* Load script if specified */
    if (file) {
        rc = ray_repl_run_file(file);
        if (!interactive && !(port > 0)) goto done;
    }

    /* REPL or pure server mode.
     *
     * Auto-detect server-only when launched as a daemon-style service:
     * `-p` set, no script, no `-i`, and neither stdin nor stdout is a
     * terminal.  In that case we skip ray_repl_create entirely and just
     * drive the poll loop — which is the right shape for `nohup ... &`,
     * systemd `Type=simple`, Docker without `-it`, etc.
     *
     * Without this guard the local REPL would enter run_piped() and
     * block on fgets, starving the same poll loop the IPC listener is
     * registered on — so connections get accepted by the kernel but
     * never user-space handled.  Auto-detect fixes the deploy hole;
     * `-i` overrides for the rare case of "I want REPL even with stdin
     * piped". */
    bool stdin_tty  = isatty(STDIN_FILENO);
    bool stdout_tty = isatty(STDOUT_FILENO);
    bool server_only = poll && port > 0 && !file && !interactive
                       && !stdin_tty && !stdout_tty;

    if (server_only) {
        /* In service deployments stdout/stderr are typically pointed
         * at a journal pipe or log file by the supervisor (systemd,
         * docker, nohup).  Anything (println …) prints from the
         * server side ends up there — see --help "Running as a
         * service" for guidance.  We deliberately don't auto-create
         * a log file: that would conflict with whatever the admin
         * already configured. */
        /* Suppress SIGPIPE so a write to a stdout/stderr pipe whose
         * peer has gone away (journald restart, log rotator, etc.)
         * doesn't kill the daemon.  Writes still fail with EPIPE,
         * libc sets ferror(stdout), and the bytes are simply lost —
         * which is the right behaviour for a service whose log sink
         * is recoverable.  Only set in server-only mode; an
         * interactive REPL with a broken pipe is genuinely fatal. */
#ifndef RAY_OS_WINDOWS
        signal(SIGPIPE, SIG_IGN);
#endif
        fprintf(stderr, "no terminal — running in server-only mode\n");
        ray_poll_run(poll);
    } else {
        ray_repl_t* repl = ray_repl_create(poll);
        if (repl) {
            ray_repl_run(repl);
            ray_repl_destroy(repl);
        } else if (poll && port > 0) {
            /* REPL create failed (OOM etc.) — fall back to pure poll. */
            ray_poll_run(poll);
        }
    }

done:
    /* Flush + close the journal before the rest of the runtime tears
     * down — fclose triggers a final fflush, which is what `-l` mode
     * relies on for "best-effort" durability at clean shutdown. */
    ray_journal_close();
    if (poll) ray_poll_destroy(poll);
    ray_runtime_destroy(rt);
    return rc;
}
