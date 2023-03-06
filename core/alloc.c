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

extern nil_t *storm_realloc(nil_t *ptr, i32_t size)
{
    return realloc(ptr, size);
}

extern nil_t storm_alloc_init()
{
}

extern nil_t storm_alloc_deinit()
{
    // munmap(GLOBAL_A0, sizeof(struct alloc_t));
}
