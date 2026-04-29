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

#include "core/poll.h"
#include "core/ipc.h"
#include "core/pool.h"
#include "core/profile.h"
#include "app/repl.h"
#include "core/runtime.h"
#include <rayforce.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

int main(int argc, char** argv) {
    ray_runtime_t* rt = ray_runtime_create(argc, argv);
    if (!rt) { fprintf(stderr, "failed to create runtime\n"); return 1; }

    int rc = 0;
    int interactive = 0;
    const char* file = NULL;
    uint16_t port = 0;
    int n_cores = -1;      /* -1 = leave pool at lazy ncpu-1 default */
    int timeit_init = 0;   /* -t N: enable profiler at startup */
    const char* auth_pw = NULL;
    bool auth_restricted = false;

    /* Parse args. Supported flags:
     *   -f FILE          run script file
     *   -p PORT          IPC listen port
     *   -c N             worker-pool size (0 = auto: ncpu - 1)
     *   -t N             enable timeit at startup (N != 0 turns on)
     *   -r N             repl mode (1 = enabled, 0 = disabled)
     *   -i               interactive
     * Plus rayforce2-specific:
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
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fprintf(stdout,
                "usage: %s [-f file] [-p port] [-c cores] [-t 0|1] [-i]"
                " [-u PW | -U PW] [file.rfl]\n"
                "  -f, --file FILE     run script file (or pass as a positional arg)\n"
                "  -p, --port PORT     listen for IPC clients on PORT\n"
                "  -c, --cores N       worker-pool size (0 = auto: ncpu - 1, default)\n"
                "  -t, --timeit N      enable profiler at startup (N != 0)\n"
                "  -i, --interactive   start the REPL even after running a file\n"
                "  -u PW               set plain auth password\n"
                "  -U PW               set restricted auth password\n"
                "  -h, --help          show this message\n",
                argv[0]);
            ray_runtime_destroy(rt);
            return 0;
        }
        else
            file = argv[i];
    }

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

    ray_poll_t* poll = ray_poll_create();

    if (poll && auth_pw) {
        size_t pw_len = strlen(auth_pw);
        if (pw_len >= sizeof(poll->auth_secret))
            pw_len = sizeof(poll->auth_secret) - 1;
        memcpy(poll->auth_secret, auth_pw, pw_len);
        poll->auth_secret[pw_len] = '\0';
        poll->restricted = auth_restricted;
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

    /* REPL or pure server mode */
    {
        ray_repl_t* repl = ray_repl_create(poll);
        if (repl) {
            ray_repl_run(repl);
            ray_repl_destroy(repl);
        } else if (poll && port > 0) {
            /* No REPL possible — run pure server loop */
            ray_poll_run(poll);
        }
    }

done:
    if (poll) ray_poll_destroy(poll);
    ray_runtime_destroy(rt);
    return rc;
}
