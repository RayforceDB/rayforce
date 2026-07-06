/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

/* Embeddable append-only log (AOF) — generic durable event log for host
 * applications embedding Rayforce.
 *
 * This is NOT the transaction journal (store/journal.h).  The journal
 * records IPC eval frames and replays them through the interpreter; a
 * torn tail there is fatal by design because entries are commands with
 * side effects.  The AOF stores opaque application payloads with
 * explicit commit barriers: a torn tail is, by construction, data the
 * writer was never acknowledged for — recovery truncates it and the LSN
 * sequence continues.  `ray_aof_commit()` is the durability line.
 *
 * On-disk layout: one directory per log; append-only segment files named
 * by the LSN of their first record (`%020lld.aof`), rotated once a
 * segment exceeds the configured byte limit.  Record framing:
 *
 *     u32 LE payload length | u32 LE CRC32(payload) | payload
 *
 * LSNs are monotonic and contiguous across segments.
 *
 * Threading: one writer handle per directory (the module does not lock;
 * a second concurrent writer corrupts the log).  Any number of
 * concurrent scans are safe, including while a writer is active — a scan
 * observes the committed prefix and stops cleanly at the first
 * incomplete record of the tail segment.  The module is independent of
 * the ray_runtime_t lifecycle and safe to use before runtime creation.
 *
 * Public API lives in include/rayforce.h ("Append-Only Log" section);
 * this header only re-exports it for in-tree consumers.
 */
#ifndef RAY_AOF_H
#define RAY_AOF_H

#include <rayforce.h>

#endif /* RAY_AOF_H */
