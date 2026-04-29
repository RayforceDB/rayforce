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
#ifndef RAY_RUNTIME_H
#define RAY_RUNTIME_H

#include <rayforce.h>

/* ===== Error Info (per-VM, ephemeral) ===== */

typedef struct {
    char msg[256];
} ray_err_info_t;

/* ===== Scope Frame (moved from env.c) ===== */

#define RAY_SCOPE_CAP  64
#define RAY_FRAME_CAP  64

typedef struct {
    int64_t keys[RAY_FRAME_CAP];
    ray_t*  vals[RAY_FRAME_CAP];
    int32_t count;
} ray_scope_frame_t;

/* ===== VM sub-types ===== */

#define RAY_VM_STACK_SIZE 1024
#define RAY_VM_TRAP_SIZE  16

typedef struct {
    ray_t   *fn;
    int32_t  fp;
    int32_t  ip;
} ray_vm_ctx_t;

typedef struct {
    int32_t  rp;
    int32_t  sp;
    int32_t  handler_ip;
    ray_t   *fn;
    int32_t  fp;
    int32_t  n_locals;
} ray_vm_trap_t;

/* ===== Per-thread VM ===== */

typedef struct {
    /* hot path */
    int32_t          sp;
    int32_t          fp;
    int32_t          rp;
    int32_t          id;
    ray_t           *fn;
    void            *heap;
    int32_t          tp;
    /* stacks */
    ray_t           *ps[RAY_VM_STACK_SIZE];
    ray_vm_ctx_t     rs[RAY_VM_STACK_SIZE];
    ray_vm_trap_t    ts[RAY_VM_TRAP_SIZE];
    /* cold — error/debug */
    ray_err_info_t   err;
    ray_t           *nfo;
    ray_t           *trace;
    ray_t           *raise_val;
    /* scope */
    ray_scope_frame_t scope_stack[RAY_SCOPE_CAP];
    int32_t          scope_depth;
} ray_vm_t;

/* ===== Runtime ===== */

typedef struct ray_runtime_s {
    ray_vm_t       **vms;
    int32_t          n_vms;
    int64_t          mem_budget;   /* 80% of physical RAM, bytes */
} ray_runtime_t;

/* Global runtime + per-thread VM */
extern ray_runtime_t *__RUNTIME;
extern _Thread_local ray_vm_t *__VM;

/* Lifecycle */
ray_runtime_t* ray_runtime_create(int argc, char** argv);
void           ray_runtime_destroy(ray_runtime_t* rt);

/* Persistent-consumer lifecycle: load the sym table from `sym_path` (if
 * present) before builtins register, so user-interned IDs keep the same
 * slots across process restarts.  The _err variant surfaces the load
 * result via `out_sym_err` (RAY_OK / RAY_ERR_CORRUPT / I/O errors) so
 * callers can decide recovery policy; the plain variant discards it. */
ray_runtime_t* ray_runtime_create_with_sym(const char* sym_path);
ray_runtime_t* ray_runtime_create_with_sym_err(const char* sym_path,
                                               ray_err_t* out_sym_err);

/* Error API — allocates ray_t with type=RAY_ERROR, sets __VM->err.msg */
ray_t* ray_error(const char* code, const char* fmt, ...);
/* Read error code from a RAY_ERROR object (returns pointer to sdata) */
const char* ray_err_code(ray_t* err);
/* ray_error_free() is published in include/rayforce.h */

/* Read VM error detail message (NULL if empty) */
const char* ray_error_msg(void);

/* Clear VM error detail */
void ray_error_clear(void);

#endif /* RAY_RUNTIME_H */
