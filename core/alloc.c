#include <stdio.h>
#include <string.h>
#include "alloc.h"
#include "storm.h"
#include <sys/mman.h>
#include <assert.h>

#include <stdlib.h>

#define MAP_ANONYMOUS 0x20

// Global allocator reference
alloc_t GLOBAL_A0 = NULL;

extern nil_t *storm_malloc(int size)
{
    return malloc(size);
}

extern nil_t storm_free(void *block)
{
    free(block);
}

extern nil_t storm_alloc_init()
{
    // printf("a0_init:\n\
    // -- PAGE_SIZE: %d\n\
    // -- POOL_SIZE: %d\n",
    //        PAGE_SIZE, POOL_SIZE);
    // alloc_t alloc;

    // alloc = (alloc_t)mmap(NULL, sizeof(struct alloc_t),
    //                       PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    // assert((alloc != MAP_FAILED));

    // memset(alloc, 0, sizeof(struct alloc_t));
    // GLOBAL_A0 = alloc;
    // GLOBAL_A0->freelist[MAX_ORDER] = GLOBAL_A0->pool;
}

extern nil_t storm_alloc_deinit()
{
    // munmap(GLOBAL_A0, sizeof(struct alloc_t));
}