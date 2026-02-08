#include "ctx.h"
#include "heap.h"
#include "eval.h"
#include "pool.h"
#include "runtime.h"
#include "mmap.h"
#include "log.h"

#include <assert.h>

ctx_registry_t __ctx_registry;

nil_t ctx_registry_init(nil_t) {
    __ctx_registry.lock = mutex_create();
    __ctx_registry.count = 0;
    memset(__ctx_registry.entries, 0, sizeof(__ctx_registry.entries));
}

nil_t ctx_registry_destroy(nil_t) {
    i64_t i;
    ray_ctx_p ctx;

    mutex_lock(&__ctx_registry.lock);
    for (i = 0; i < __ctx_registry.count; i++) {
        ctx = __ctx_registry.entries[i];
        if (ctx != NULL && ctx->heap != NULL) {
            LOG_WARN("Leaked custom thread context id=%lld — force merging heap", ctx->id);
            heap_push_pending(ctx->heap);
        }
        if (ctx != NULL)
            mmap_free(ctx, sizeof(struct ray_ctx_t));
        __ctx_registry.entries[i] = NULL;
    }
    __ctx_registry.count = 0;
    mutex_unlock(&__ctx_registry.lock);

    // Drain any pending heaps so they get merged before final cleanup
    heap_drain_pending();

    mutex_destroy(&__ctx_registry.lock);
}

ray_ctx_p ray_ctx_create(nil_t) {
    ray_ctx_p ctx;
    i64_t id;
    vm_p vm;

    assert(__VM == NULL && "ray_ctx_create: thread already has a VM");

    id = heap_next_id();

    // Create VM + heap for this thread (sets __VM)
    vm = vm_create(id, __RUNTIME->pool);
    vm->rc_sync = 1;  // Always use atomic RC for custom threads

    // Register in global registry
    mutex_lock(&__ctx_registry.lock);
    if (__ctx_registry.count >= MAX_CUSTOM_THREADS) {
        mutex_unlock(&__ctx_registry.lock);
        LOG_ERROR("ray_ctx_create: too many custom threads (max %d)", MAX_CUSTOM_THREADS);
        vm_destroy(vm);
        return NULL;
    }
    assert(__ctx_registry.count < MAX_CUSTOM_THREADS && "ray_ctx_create: too many custom threads");

    ctx = (ray_ctx_p)mmap_alloc(sizeof(struct ray_ctx_t));
    if (ctx == NULL) {
        mutex_unlock(&__ctx_registry.lock);
        LOG_ERROR("ray_ctx_create: failed to allocate context");
        vm_destroy(vm);
        return NULL;
    }

    ctx->id = id;
    ctx->heap = vm->heap;
    ctx->vm = vm;
    __ctx_registry.entries[__ctx_registry.count++] = ctx;
    mutex_unlock(&__ctx_registry.lock);

    return ctx;
}

nil_t ray_ctx_destroy(ray_ctx_p ctx) {
    i64_t i;
    vm_p vm;
    heap_p heap;
    b8_t found = B8_FALSE;

    assert(__VM == ctx->vm && "ray_ctx_destroy: called from wrong thread");

    vm = ctx->vm;
    heap = ctx->heap;

    // Flush own foreign blocks
    heap_flush_foreign(heap);

    // Flush slab caches to freelists
    heap_flush_slabs(heap);

    // Clear VM stack objects
    while (vm->sp > 0)
        drop_obj(vm->ps[--vm->sp]);

    // Free VM-owned resources
    if (vm->timeit) {
        heap_free(vm->timeit);
        vm->timeit = NULL;
    }

    // Push heap to pending merge queue (main thread will drain it)
    heap_push_pending(heap);

    // Detach heap from VM before destroying VM (heap stays alive for main to drain)
    vm->heap = NULL;
    __VM = NULL;
    mmap_free(vm, sizeof(struct vm_t));

    // Unregister from global registry
    mutex_lock(&__ctx_registry.lock);
    for (i = 0; i < __ctx_registry.count; i++) {
        if (__ctx_registry.entries[i] == ctx) {
            // Swap with last entry
            __ctx_registry.entries[i] = __ctx_registry.entries[--__ctx_registry.count];
            found = B8_TRUE;
            break;
        }
    }
    mutex_unlock(&__ctx_registry.lock);

    if (!found) {
        LOG_WARN("ray_ctx_destroy: context not found in registry (id=%lld)", ctx->id);
        return;
    }

    mmap_free(ctx, sizeof(struct ray_ctx_t));
}
