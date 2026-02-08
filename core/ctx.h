#ifndef CTX_H
#define CTX_H

#include "rayforce.h"
#include "thread.h"

#define MAX_CUSTOM_THREADS 64

struct heap_t;
struct vm_t;

typedef struct ray_ctx_t {
    i64_t id;
    struct heap_t *heap;
    struct vm_t *vm;
} *ray_ctx_p;

typedef struct ctx_registry_t {
    mutex_t lock;
    i64_t count;
    ray_ctx_p entries[MAX_CUSTOM_THREADS];
} ctx_registry_t;

// Global registry (initialized during runtime_create)
extern ctx_registry_t __ctx_registry;

nil_t ctx_registry_init(nil_t);
nil_t ctx_registry_destroy(nil_t);

// Public API — call ON the custom thread
ray_ctx_p ray_ctx_create(nil_t);
nil_t ray_ctx_destroy(ray_ctx_p ctx);

#endif  // CTX_H
