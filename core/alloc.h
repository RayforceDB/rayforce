#ifndef ALLOC_H_
#define ALLOC_H_

#include "storm.h"

#define PAGE_SIZE 4096
#define MIN_ORDER 2
#define MAX_ORDER 10
#define MIN_ALLOC ((i64_t)1 << MIN_ORDER)
#define MAX_ALLOC ((i64_t)1 << MAX_ORDER)
#define POOL_SIZE (MAX_ALLOC * 2)
#define BUCKET_COUNT (MAX_ORDER - MIN_ORDER + 1)

typedef struct alloc_t
{
    nil_t *freelist[BUCKET_COUNT];
    i8_t pool[POOL_SIZE];
} __attribute__((aligned(PAGE_SIZE))) * alloc_t;

CASSERT(sizeof(struct alloc_t) % PAGE_SIZE == 0, alloc_h)

extern nil_t *storm_malloc(i32_t size);
extern nil_t *storm_realloc(nil_t *ptr, i32_t size);
extern nil_t storm_free(nil_t *block);

extern nil_t storm_alloc_init();
extern nil_t storm_alloc_deinit();

#endif
