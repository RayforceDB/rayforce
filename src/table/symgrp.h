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

#ifndef RAY_SYMGRP_H
#define RAY_SYMGRP_H

/* The import dictionary's grouped mode: interning a column too big for its
 * dictionary to be probed at random.
 *
 * A pass over one column runs in three steps, the caller (the Parquet
 * import) decoding the column twice:
 *
 *   1. every task (a row group's chunk) deduplicates its strings exactly and
 *      hands its distinct ones over as (hash, length, local id) only; no
 *      string bytes are kept between the steps;
 *   2. ray_symgrp_resolve goes through the hash groups one at a time, the
 *      group's index (every earlier string's hash, length and position, kept
 *      in append-only logs) the only table it holds, and gives each staged
 *      string a verdict: a new string at a position of its own, or a
 *      candidate for an earlier string of equal hash and length;
 *   3. the tasks decode again, write the new strings' records where step 2
 *      placed them and compare every candidate's bytes with its record,
 *      length first and then bytes to the first that differs.  A candidate
 *      pointing at an earlier pass (or an earlier window of tasks) is
 *      compared with a copy of its record read beforehand in position
 *      order (ray_symgrp_load); one pointing at a record another task of the
 *      window writes is compared once the window is done (ray_symgrp_defer,
 *      ray_symgrp_settle).  A mismatch, a true hash collision, is kept for
 *      the window's end too (ray_symgrp_mismatch): ray_symgrp_settle gives
 *      the window's mismatches, in the order their strings first occur, their
 *      exact positions from the logs, new ones if need be, and their tasks
 *      write their codes again.  Two different strings never share a
 *      position, and the positions do not depend on the workers.
 *
 * Nothing reads the symbol file at random but those collisions, and the
 * index is touched one group at a time.  POSIX only, as the import
 * dictionary itself. */

#include <rayforce.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ray_symimp_s;
typedef struct ray_symgrp_s ray_symgrp_t;

#define RAY_SYMGRP_LOG_BITS 6                     /* the index logs: 64, by the hash's top bits */
#define RAY_SYMGRP_MAX      (1 << RAY_SYMGRP_LOG_BITS)

/* The order a pass's new strings take positions in. */
typedef enum {
    RAY_SYMGRP_ROWS   = 0,   /* first occurrence, the strings later tasks meet
                              * again first: those, task by task in their rows'
                              * order, then the others the same way (the
                              * records a later window compares with lie
                              * together, not spread over the pass's) */
    RAY_SYMGRP_SHARDS = 1,   /* hash group, then task, then first occurrence */
    RAY_SYMGRP_FREQ   = 2,   /* most rows first; ties by first occurrence */
    RAY_SYMGRP_ROWS_FLAT = 3,/* first occurrence: task by task, each in its rows' order */
} ray_symgrp_order_t;

/* A task-distinct string as step 1 saw it; local ids count from 1 (0 is the
 * null and "" row, position 0, never staged). */
typedef struct { uint64_t h; uint32_t len, local; } ray_symgrp_fp_t;

/* Step 2's verdict on one of a task's local ids. */
enum { RAY_SYMGRP_NEW = 1, RAY_SYMGRP_OLD = 2, RAY_SYMGRP_REF = 3 };
typedef struct {
    uint32_t pos;     /* the position; for OLD and REF until the bytes are compared */
    uint32_t kind;
    uint32_t owner;   /* REF: the task whose NEW string it is */
    uint32_t again;   /* NEW under ROWS: a later task meets it too */
    int64_t  off;     /* NEW: its record's file offset (-1 under ROWS and
                       * ROWS_FLAT: the task's records follow one another,
                       * in local id order, from ray_symgrp_task_off of
                       * `again`) */
} ray_symgrp_res_t;

typedef struct {
    int64_t staged, groups, log_loaded, owners, old, refs;
    int64_t again, again_bytes;                    /* ROWS: new strings later tasks meet */
    int64_t store_pos, store_spans, store_bytes;   /* records read from the file */
    int64_t store_read, settle_read;                /* bytes asked of the file for them */
    int64_t store_kept, windows;                    /* records kept from the window before */
    int64_t stage_bytes;                            /* the tasks' arrays: their chunks' bytes */
    int64_t rec_bytes, wb_bytes;                    /* records written; bytes whose writeback the windows started */
    int64_t deferred, compares, cmp_bytes, collisions, redo;
} ray_symgrp_stats_t;

ray_symgrp_t* ray_symgrp_new(struct ray_symimp_s* imp, ray_symgrp_order_t order);
void ray_symgrp_free(ray_symgrp_t* g);
ray_symgrp_order_t ray_symgrp_order(const ray_symgrp_t* g);

/* A pass over `ntasks` tasks in `groups` hash groups (a power of two up to
 * RAY_SYMGRP_MAX); `workers` bounds the worker ids given to ray_symgrp_defer. */
bool ray_symgrp_begin(ray_symgrp_t* g, int64_t ntasks, int groups, int64_t workers);
/* Step 1: task t's distinct strings, fp[i].local == i + 1; counts[i] the
 * rows of local id i + 1 (RAY_SYMGRP_FREQ; NULL otherwise).  Thread-safe
 * across tasks. */
bool ray_symgrp_stage(ray_symgrp_t* g, int64_t t, int64_t n, const ray_symgrp_fp_t* fp,
                      const uint32_t* counts);
/* Step 2.  Reserves the new strings' records in the symbol file and appends
 * them to the index logs. */
bool ray_symgrp_resolve(ray_symgrp_t* g);
/* Step 3: task t's verdicts, res[local] for local ids 1..n (n as staged). */
int64_t ray_symgrp_task_size(const ray_symgrp_t* g, int64_t t);
bool ray_symgrp_task(const ray_symgrp_t* g, int64_t t, ray_symgrp_res_t* res);
/* RAY_SYMGRP_ROWS, ROWS_FLAT: the file offset of task t's first new record,
 * of those later tasks meet again (`again`) or of the others. */
int64_t ray_symgrp_task_off(const ray_symgrp_t* g, int64_t t, bool again);
/* The window of tasks [ta, return value) whose candidates' records stay
 * within `budget` bytes (one task at least). */
int64_t ray_symgrp_window(const ray_symgrp_t* g, int64_t ta, int64_t budget);
/* Read the records the window's candidates need from before the window, in
 * position order, into memory. */
bool ray_symgrp_load(ray_symgrp_t* g, int64_t ta, int64_t tb);
/* Whether `s` is the string at `pos`: the loaded copy if any, else the
 * record in the file (written by now). */
bool ray_symgrp_same(ray_symgrp_t* g, uint32_t pos, const char* s, uint32_t len);
/* A candidate pointing at a record another task of the window writes:
 * compared by ray_symgrp_settle.  `worker` < the workers of begin. */
bool ray_symgrp_defer(ray_symgrp_t* g, uint32_t worker, int64_t t, uint32_t local, uint64_t h,
                      uint32_t pos, const char* s, uint32_t len);
/* A candidate compared already that did not match: its position is given
 * by ray_symgrp_settle.  `worker` as for ray_symgrp_defer. */
bool ray_symgrp_mismatch(ray_symgrp_t* g, uint32_t worker, int64_t t, uint32_t local, uint64_t h,
                         const char* s, uint32_t len);
/* After the window: compare the deferred candidates in position order, then
 * give every mismatch (those and ray_symgrp_mismatch's), by task and local
 * id, its exact position: an earlier string of equal hash and length, or a
 * new record.  The positions are kept for ray_symgrp_override, and the
 * mismatches' tasks listed in *redo (*nredo tasks, ascending) to write their
 * codes again. */
bool ray_symgrp_settle(ray_symgrp_t* g, int64_t** redo, int64_t* nredo);
/* The position settled for (t, local) after a mismatch; -1 if none. */
int64_t ray_symgrp_override(const ray_symgrp_t* g, int64_t t, uint32_t local);
/* After the window and its codes written again: start the writeback of the
 * records it wrote (and of those its collisions added). */
void ray_symgrp_writeback(ray_symgrp_t* g, int64_t ta, int64_t tb);
/* Step 3's own comparisons, for the counts. */
void ray_symgrp_note(ray_symgrp_t* g, int64_t compares, int64_t bytes);
/* End of the pass: its state freed, the logs kept for the next. */
void ray_symgrp_end(ray_symgrp_t* g);
/* Counts since the last call (which resets them). */
void ray_symgrp_stats(ray_symgrp_t* g, ray_symgrp_stats_t* out);
/* Entries in the index logs. */
int64_t ray_symgrp_entries(const ray_symgrp_t* g);

#endif
