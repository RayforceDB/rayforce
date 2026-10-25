#ifndef RAY_READ_BUDGET_H
#define RAY_READ_BUDGET_H
#include <rayforce.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct ray_read_records ray_read_records_t;
typedef enum { RAY_READ_OK, RAY_READ_LIMIT, RAY_READ_OOM, RAY_READ_INVALID } ray_read_status_t;
/* Internal proof context, not a Rayfall API. Values are borrowed until retained
 * by the caller. close releases the context's refs, including transitive work.
 * No global allocator hook or ambient budget is installed. */
typedef struct ray_read_budget {
    size_t limit, live, peak, attempts, fail_after;
    ray_read_status_t status;
    ray_read_records_t* records;
} ray_read_budget_t;

bool ray_read_budget_init(ray_read_budget_t* b, size_t limit);
void ray_read_budget_close(ray_read_budget_t* b);
void* ray_read_sys_alloc(ray_read_budget_t* b, size_t bytes);
void* ray_read_sys_resize(ray_read_budget_t* b, void* old, size_t old_bytes, size_t bytes);
void ray_read_sys_free(ray_read_budget_t* b, void* p);
/* Transfer a preallocated block to permanent storage, after nonfailing commit. */
void ray_read_sys_transfer(ray_read_budget_t* b, void* p);
ray_t* ray_read_alloc(ray_read_budget_t* b, size_t bytes);
void ray_read_release(ray_read_budget_t* b, ray_t* v);
ray_t* ray_read_string(ray_read_budget_t* b, const char* s, size_t n);
ray_t* ray_read_i64(ray_read_budget_t* b, const int64_t* values, size_t n);
ray_t* ray_read_list(ray_read_budget_t* b, ray_t* const* values, size_t n);
#endif
