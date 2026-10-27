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

#ifndef RAY_SHM_H
#define RAY_SHM_H

#include "core/poll.h"

/* ===== The same-host transport =====
 *
 * Two processes on one host share a memory file holding a ring per
 * direction.  A writer copies a frame's bytes into its ring and moves the
 * head; a reader copies them out and moves the tail.  A side that finds
 * nothing to do says it sleeps and waits on its eventfd; the other side
 * writes to that eventfd only then, so a busy pair makes no system calls.
 *
 * The server creates the file and two eventfds and passes all three
 * descriptors to the client over a Unix-domain socket (core/ipc.c); each
 * side then maps the same file.  Linux only (memfd, eventfd); elsewhere
 * nothing is available and every connection stays on its socket. */

/* This boot of this host, as the kernel names it (36 characters): two
 * processes that see the same identity are on one host.  Empty when the
 * host does not tell. */
const char* ray_shm_host_identity(void);
#define RAY_SHM_IDENTITY_LEN 36

/* Bytes of each direction's ring. */
uint64_t ray_shm_ring_bytes(void);
#define RAY_SHM_RING_DEFAULT (16ULL << 20)
#define RAY_SHM_RING_MAX     (1ULL << 30)

/* The server's half: the memory file and two eventfds, in `fds` for the
 * client (the file, the eventfd the client sleeps on, the one the server
 * sleeps on); the server keeps its own duplicates.  NULL when the host
 * refuses (then nothing is in fds). */
ray_xport_t* ray_shm_create(uint64_t ring_bytes, int fds[3]);
/* The client's half, from the descriptors the server passed; consumes
 * them, on failure too. */
ray_xport_t* ray_shm_attach(uint64_t ring_bytes, const int fds[3]);

#endif /* RAY_SHM_H */
