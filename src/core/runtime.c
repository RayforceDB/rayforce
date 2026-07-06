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

#include "runtime.h"
#include "core/profile.h"   /* g_ray_profile rings — dropped on destroy */
#include "mem/heap.h"
#include "mem/sys.h"
#include "table/sym.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#ifdef RAY_OS_WINDOWS
#include <windows.h>
#else
#include <unistd.h>
#endif

/* Forward-declare lang init/destroy — keeps runtime.c decoupled from lang/eval.h */
extern ray_err_t ray_lang_init(void);
extern void      ray_lang_destroy(void);

/* ===== Global state ===== */

ray_runtime_t *__RUNTIME = NULL;
_Thread_local ray_vm_t *__VM = NULL;

/* Persistent error message buffer.
 *
 * `__VM->err.msg` is the canonical per-thread slot now that the VM is
 * stable for the thread's lifetime, but FFI callers can hit errors on
 * threads that never bound a VM — this thread-local shadow keeps
 * ray_error_msg() answerable there too. */
static _Thread_local char ray_last_err_msg[256] = {0};

/* Zero a VM and set the non-zero defaults.  Every VM must go through
 * here — a bare memset leaves ipc_handle at 0, which is a VALID
 * connection handle; the "no dispatch on this stack" sentinel is -1. */
void ray_vm_init(ray_vm_t* vm, int32_t id) {
    memset(vm, 0, sizeof(*vm));
    vm->id = id;
    vm->ipc_handle = -1;
}

/* Static null singleton — type RAY_NULL, ARENA flag makes retain/release no-ops */
ray_t __ray_null = { .type = RAY_NULL, .attrs = RAY_ATTR_ARENA, .rc = 0, .len = 0 };

/* Static last-resort OOM error — used when ray_error itself can't allocate
 * the small block it needs to construct a fresh error.  Tagged with
 * RAY_ATTR_ARENA so retain/release are no-ops, matching __ray_null.  Code
 * is "oom" inline (slen=3).  Per-VM message is dropped under deep OOM since
 * we have no heap to format anything new — callers get the bare type/code.
 *
 * Without this sentinel, hard OOM (heap can't satisfy even the 32-byte
 * error header) makes ray_error return NULL, which silently bypasses
 * every `if (RAY_IS_ERR(x)) return x;` guard upstream and reintroduces
 * exactly the silent-failure pathology the wrapper-level fixes were
 * meant to close. */
ray_t __ray_oom = {
    .type  = RAY_ERROR,
    .attrs = RAY_ATTR_ARENA,
    .rc    = 0,
    /* slen / sdata share a union with len / i64 / etc. (bytes 16-31) —
     * pick one designated path for that union or clang's
     * -Winitializer-overrides flags it under -Werror. */
    .slen  = 3,
    .sdata = { 'o', 'o', 'm', 0, 0, 0, 0 },
};

/* ===== Error code to string ===== */

const char* ray_err_code_str(ray_err_t e) {
    static const char* codes[] = {
        [RAY_OK]          = "ok",
        [RAY_ERR_OOM]     = "oom",
        [RAY_ERR_TYPE]    = "type",
        [RAY_ERR_RANGE]   = "range",
        [RAY_ERR_LENGTH]  = "length",
        [RAY_ERR_RANK]    = "rank",
        [RAY_ERR_DOMAIN]  = "domain",
        [RAY_ERR_NYI]     = "nyi",
        [RAY_ERR_IO]      = "io",
        [RAY_ERR_SCHEMA]  = "schema",
        [RAY_ERR_CORRUPT] = "corrupt",
        [RAY_ERR_CANCEL]  = "cancel",
        [RAY_ERR_PARSE]   = "parse",
        [RAY_ERR_NAME]    = "name",
        [RAY_ERR_LIMIT]   = "limit",
        /* "reserve" (not "reserved") because the err->sdata inline field
         * is capped at 7 bytes — the past-tense form would truncate. */
        [RAY_ERR_RESERVED] = "reserve",
        [RAY_ERR_VERSION] = "version",
    };
    if ((unsigned)e >= sizeof(codes)/sizeof(codes[0])) return "error";
    return codes[e];
}

ray_err_t ray_err_from_obj(ray_t* err) {
    if (!err || err->type != RAY_ERROR) return RAY_ERR_DOMAIN;
    const char* s = err->sdata;
    int n = err->slen;
    static const struct { const char* s; int len; ray_err_t e; } map[] = {
        {"oom",     3, RAY_ERR_OOM},     {"type",    4, RAY_ERR_TYPE},
        {"range",   5, RAY_ERR_RANGE},   {"length",  6, RAY_ERR_LENGTH},
        {"rank",    4, RAY_ERR_RANK},    {"domain",  6, RAY_ERR_DOMAIN},
        {"nyi",     3, RAY_ERR_NYI},     {"io",      2, RAY_ERR_IO},
        {"schema",  6, RAY_ERR_SCHEMA},  {"corrupt", 7, RAY_ERR_CORRUPT},
        {"cancel",  6, RAY_ERR_CANCEL},  {"parse",   5, RAY_ERR_PARSE},
        {"name",    4, RAY_ERR_NAME},    {"limit",   5, RAY_ERR_LIMIT},
        {"reserve", 7, RAY_ERR_RESERVED},
        {"version", 7, RAY_ERR_VERSION},
    };
    for (int i = 0; i < (int)(sizeof(map)/sizeof(map[0])); i++)
        if (n == map[i].len && memcmp(s, map[i].s, n) == 0) return map[i].e;
    return RAY_ERR_DOMAIN;
}

/* ===== Error API ===== */

static ray_t* ray_verror(const char* code, const char* fmt, va_list ap) {
    /* Populate / clear the persistent message buffer FIRST.  On the
     * deep-OOM path below we return the static __ray_oom sentinel, but
     * that path still has to leave ray_error_msg() consistent with this
     * call.  The buffer is thread-local storage, so this does not depend
     * on the heap and stays valid even when ray_alloc below fails. */
    if (fmt) {
        va_list copy;
        va_copy(copy, ap);
        vsnprintf(ray_last_err_msg, sizeof(ray_last_err_msg), fmt, copy);
        va_end(copy);
        if (__VM)
            memcpy(__VM->err.msg, ray_last_err_msg, sizeof(__VM->err.msg));
    } else {
        ray_last_err_msg[0] = '\0';
        if (__VM) __VM->err.msg[0] = '\0';
    }

    ray_t* err = ray_alloc(0);
    if (!err) return &__ray_oom;  /* sentinel — see __ray_oom comment */
    err->type = RAY_ERROR;
    err->slen = 0;
    memset(err->sdata, 0, 7);
    if (code) {
        size_t len = strlen(code);
        if (len > 7) len = 7;
        memcpy(err->sdata, code, len);
        err->slen = (uint8_t)len;
    }
    return err;
}

ray_t* ray_error(const char* code, const char* fmt, ...) {
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        ray_t* err = ray_verror(code, fmt, ap);
        va_end(ap);
        return err;
    }
    /* No format string — skip va_list entirely for portability.  Clear
     * the persistent message buffer FIRST so the deep-OOM sentinel path
     * doesn't leave stale text from an earlier error visible. */
    ray_last_err_msg[0] = '\0';
    if (__VM) __VM->err.msg[0] = '\0';
    ray_t* err = ray_alloc(0);
    if (!err) return &__ray_oom;  /* sentinel — see __ray_oom comment */
    err->type = RAY_ERROR;
    err->slen = 0;
    memset(err->sdata, 0, 7);
    if (code) {
        size_t len = strlen(code);
        if (len > 7) len = 7;
        memcpy(err->sdata, code, len);
        err->slen = (uint8_t)len;
    }
    return err;
}

void ray_error_free(ray_t* err) {
    /* Skip NULL and anything that isn't actually a RAY_ERROR — callers
     * often pass a result that might be either an error or a real value. */
    if (!err || !RAY_IS_ERR(err)) return;
    /* The static OOM sentinel lives in BSS, not the heap.  Freeing it
     * would corrupt the buddy allocator's bookkeeping. */
    if (err == RAY_OOM_OBJ) return;
    /* Both ray_free and ray_release_owned_refs short-circuit on RAY_IS_ERR
     * as a safety default (the refcount system deliberately does not track
     * error objects).  Retype the block to a leaf atom (-RAY_I64) so those
     * guards don't fire — an atom with no owned children is the safest
     * shape to pass through the standard free path.  The rc was already
     * 1 from ray_alloc, so ray_free will reclaim the block via the buddy
     * allocator.  From this point the caller must not touch err again. */
    err->type = -RAY_I64;
    ray_free(err);
}

const char* ray_err_code(ray_t* err) {
    if (!err || err->type != RAY_ERROR) return NULL;
    /* sdata is 7 bytes and may not be null-terminated when full */
    static _Thread_local char buf[8];
    memcpy(buf, err->sdata, err->slen);
    buf[err->slen] = '\0';
    return buf;
}

const char* ray_error_msg(void) {
    if (!ray_last_err_msg[0]) return NULL;
    return ray_last_err_msg;
}

void ray_error_clear(void) {
    ray_last_err_msg[0] = '\0';
    if (__VM) __VM->err.msg[0] = '\0';
}

/* ===== Lifecycle ===== */

static ray_runtime_t* runtime_create_impl(const char* sym_path,
                                           ray_err_t* out_sym_err) {
    if (out_sym_err) *out_sym_err = RAY_OK;

    /* Init subsystems */
    ray_heap_init();
    ray_sym_init();

    /* Allocate runtime and set __VM + total_ram BEFORE any file I/O so
     * that ray_error() has a live VM to record diagnostics against and
     * allocations are bounded by the budget. */
    ray_runtime_t* rt = (ray_runtime_t*)ray_sys_alloc(sizeof(ray_runtime_t));
    if (!rt) return NULL;
    memset(rt, 0, sizeof(*rt));

    /* Create main VM (id=0) */
    rt->n_vms = 1;
    rt->vms = (ray_vm_t**)ray_sys_alloc(sizeof(ray_vm_t*));
    if (!rt->vms) { ray_sys_free(rt); return NULL; }
    rt->vms[0] = (ray_vm_t*)ray_sys_alloc(sizeof(ray_vm_t));
    if (!rt->vms[0]) { ray_sys_free(rt->vms); ray_sys_free(rt); return NULL; }
    ray_vm_init(rt->vms[0], 0);
    __VM = rt->vms[0];

    /* Detect total physical RAM (informational — surfaced via .sys.mem and
     * used as a sanity bound for the sym-file pre-load check below). There is
     * NO enforced memory ceiling: the heap is out-of-core and spills to a
     * file-backed mapping when anonymous mmap is refused (see heap_add_pool). */
#ifdef RAY_OS_WINDOWS
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms))
        rt->total_ram = (int64_t)ms.ullTotalPhys;
    else
        rt->total_ram = (int64_t)(4ULL << 30);
#else
    long pages = sysconf(_SC_PHYS_PAGES);
    long psize = sysconf(_SC_PAGESIZE);
    if (pages > 0 && psize > 0)
        rt->total_ram = (int64_t)((double)pages * (double)psize);
    else
        rt->total_ram = (int64_t)(4ULL << 30);
#endif

    /* __RUNTIME must be visible before ray_sym_load so the sym-load size
     * check and ray_error() both operate against the live runtime. */
    __RUNTIME = rt;

    /* Load persisted symbol table BEFORE ray_lang_init interns builtins.
     * Ordering: __VM + total_ram are live so file I/O errors surface via
     * ray_error() and the sym-file size guard has a bound.  Still before
     * ray_lang_init so persisted user symbol IDs keep their slots and
     * builtins append afterwards. */
    if (sym_path) {
        /* Pre-flight size check: reject an implausibly large sym file (more
         * than half of physical RAM) before ever touching ray_col_load.
         *
         * errno handling: ENOENT is the normal first-run case and stays
         * RAY_OK; any *other* stat failure (EACCES, ENOTDIR, EIO, …) is
         * a real problem and must be surfaced as RAY_ERR_IO, otherwise
         * the caller would silently continue with an empty sym table
         * and later hit the "divergence" class of bugs this entrypoint
         * was added to avoid. */
        struct stat st;
        if (stat(sym_path, &st) == 0) {
            /* Allow the sym file itself plus some working headroom (2x).
             * A well-formed sym file is a list of interned strings; the
             * in-memory footprint is bounded by file size within a small
             * constant factor. */
            if (st.st_size > 0 &&
                (int64_t)st.st_size > rt->total_ram / 2) {
                if (out_sym_err) *out_sym_err = RAY_ERR_OOM;
                /* Continue startup with empty sym table; caller decides
                 * whether to treat this as fatal. */
            } else {
                ray_err_t sym_err = ray_sym_load(sym_path);
                if (out_sym_err) *out_sym_err = sym_err;
                /* RAY_ERR_CORRUPT and I/O errors are non-fatal here:
                 * caller inspects out_sym_err to decide recovery. */
            }
        } else if (errno != ENOENT) {
            if (out_sym_err) *out_sym_err = RAY_ERR_IO;
        }
        /* ENOENT: leave out_sym_err = RAY_OK — absent sym file is the
         * normal first-run case. */
    }

    /* Init language (env + builtins) — must be after __VM is set and
     * after sym_load so persisted user IDs keep their slots. */
    ray_lang_init();

    return rt;
}

ray_runtime_t* ray_runtime_create(int argc, char** argv) {
    (void)argc; (void)argv;
    return runtime_create_impl(NULL, NULL);
}

ray_runtime_t* ray_runtime_create_with_sym(const char* sym_path) {
    return runtime_create_impl(sym_path, NULL);
}

ray_runtime_t* ray_runtime_create_with_sym_err(const char* sym_path,
                                               ray_err_t* out_sym_err) {
    return runtime_create_impl(sym_path, out_sym_err);
}

/* ===== Main event loop accessors =====
 * The poll is opaque to runtime.h (stored as `void*`) so adding it
 * doesn't drag poll.h into every TU that includes runtime.h.  Set
 * once by main.c after ray_poll_create; read by runtime-level
 * builtins (.sys.listen, .sys.cmd "listen N"). */

void ray_runtime_set_poll(void* poll) {
    if (__RUNTIME) __RUNTIME->poll = poll;
}

void* ray_runtime_get_poll(void) {
    return __RUNTIME ? __RUNTIME->poll : NULL;
}

void ray_runtime_set_sys_args(void* dict) {
    if (!__RUNTIME) return;
    if (__RUNTIME->sys_args) ray_release((ray_t*)__RUNTIME->sys_args);
    __RUNTIME->sys_args = dict;
}

void* ray_runtime_get_sys_args(void) {
    return __RUNTIME ? __RUNTIME->sys_args : NULL;
}

/* ===== Memory Budget API ===== */

int64_t ray_sys_total_ram(void) {
    return __RUNTIME ? __RUNTIME->total_ram : 0;
}

int8_t ray_obj_type(ray_t* v) {
    return v ? v->type : 0;
}

uint8_t ray_obj_attrs(ray_t* v) {
    return v ? v->attrs : 0;
}

int64_t ray_vec_get_i64(ray_t* vec, int64_t idx) {
    if (!vec || idx < 0 || idx >= vec->len) return 0;
    if (vec->type == RAY_I64 || vec->type == RAY_TIMESTAMP) {
        return ((const int64_t*)ray_data(vec))[idx];
    }
    if (vec->type == RAY_I32 || vec->type == RAY_DATE || vec->type == RAY_TIME) {
        return ((const int32_t*)ray_data(vec))[idx];
    }
    if (vec->type == RAY_I16) return ((const int16_t*)ray_data(vec))[idx];
    if (vec->type == RAY_U8 || vec->type == RAY_BOOL) return ((const uint8_t*)ray_data(vec))[idx];
    return 0;
}

double ray_vec_get_f64(ray_t* vec, int64_t idx) {
    if (!vec || idx < 0 || idx >= vec->len) return 0.0;
    if (vec->type == RAY_F64) return ((const double*)ray_data(vec))[idx];
    if (vec->type == RAY_F32) return ((const float*)ray_data(vec))[idx];
    return 0.0;
}

int64_t ray_vec_get_sym_id(ray_t* vec, int64_t idx) {
    if (!vec || idx < 0 || idx >= vec->len) return 0;
    if (vec->type != RAY_SYM) return 0;
    return ray_read_sym(ray_data(vec), idx, vec->type, vec->attrs);
}

void ray_runtime_destroy(ray_runtime_t* rt) {
    if (!rt) return;

    ray_lang_destroy();

    /* Free VMs */
    for (int32_t i = 0; i < rt->n_vms; i++) {
        ray_vm_t* vm = rt->vms[i];
        if (vm->raise_val) ray_release(vm->raise_val);
        if (vm->trace) { ray_release(vm->trace); vm->trace = NULL; }
        ray_sys_free(vm);
    }
    ray_sys_free(rt->vms);

    if (rt->sys_args) { ray_release((ray_t*)rt->sys_args); rt->sys_args = NULL; }

    __VM = NULL;
    __RUNTIME = NULL;

    /* The profiler rings hold `const char* msg` pointers whose backing
     * (sym-arena strings, fn-object aux bytes) dies with the runtime.
     * A process that re-initializes the runtime (the test harness does)
     * must not let spans from a dead runtime survive into the next one —
     * .sys.prof would strlen dangling pointers. Drop both rings here. */
    g_ray_profile.active = false;
    g_ray_profile.n = 0;
    g_ray_profile_last.n = 0;

    ray_sym_destroy();
    ray_heap_destroy();

    ray_sys_free(rt);
}
