/*
 *   Copyright (c) 2023 Anton Kundenko <singaraiona@gmail.com>
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

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <time.h>
#include "../core/rayforce.h"
#include "../core/format.h"
#include "../core/unary.h"
#include "../core/alloc.h"
#include "../core/vm.h"
#include "../core/hash.h"
#include "../core/symbols.h"
#include "../core/string.h"
#include "../core/vector.h"
#include "../core/util.h"
#include "../core/parse.h"
#include "../core/runtime.h"

// int test_hash()
// {
//     ht_t *table = ht_new();
//     clock_t start, end;
//     f64_t cpu_time_used;

//     str_t str[1000000];

//     u64_t pg_size = 4096 * 1024;

//     u64_t *buckets = malloc(pg_size * sizeof(u64_t));
//     memset(buckets, 0, pg_size * sizeof(u64_t));

//     for (int i = 0; i < 1000000; i++)
//     {
//         str[i] = (str_t)malloc(10);
//         snprintf(str[i], 10, "%d", 100000000 + i);
//     }

//     start = clock();

//     u64_t collisions = 0;

//     for (int i = 0; i < 1000000; i++)
//     {
//         // u64_t h = hash(str[i]);

//         // if (buckets[h % pg_size] == 1)
//         //     collisions++;
//         // else
//         //     buckets[h % pg_size] = 1;

//         // printf("%s\n", str[i]);
//         ht_insert(table, str[i], i);
//     }

//     end = clock();

//     collisions = table->collisions;
//     cpu_time_used = ((f64_t)(end - start)) / CLOCKS_PER_SEC;
//     printf("Time: %f ms\n", cpu_time_used * 1000);
//     printf("Collisions: %lld  %lld%%", collisions, (collisions * 100) / 1000000);

//     ht_free(table);

//     return 0;
// }

int test_symbols()
{
    // clock_t start, end;
    // f64_t cpu_time_used;

    // str_t st[1000000];

    // i64_t pg_size = 4096 * 1024;

    // i64_t *buckets = malloc(pg_size * sizeof(i64_t));
    // memset(buckets, 0, pg_size * sizeof(i64_t));

    // for (int i = 0; i < 1000000; i++)
    // {
    //     st[i] = (str_t)malloc(10);
    //     snprintf(st[i], 10, "%d", 100000000 + i);
    // }

    // start = clock();

    // for (int i = 0; i < 1000000; i++)
    // {
    //     // printf("%s\n", st[i]);
    //     rf_object_t s = str(st[i], strlen(st[i]));
    //     i64_t id = intern_symbol(&s);
    //     // str_t val = symbols_get(id);
    //     // if (val == NULL)
    //     //     printf("NULL -- ID: %lld ORIG: %s\n", id, st[i]);
    //     // else
    //     //     printf("%s\n", val);
    // }

    // // rf_object_t s = str("code", 4);
    // // i64_t id1 = intern_symbol(&s), id2;
    // // str_t val = symbols_get(id1);

    // // s = str("code", 4);
    // // id2 = intern_symbol(&s);
    // // val = symbols_get(id2);

    // // printf("%d == %d\n", id1, id2);

    // end = clock();

    // cpu_time_used = ((f64_t)(end - start)) / CLOCKS_PER_SEC;
    // printf("Time: %f ms\n", cpu_time_used * 1000);
    return 0;
}

null_t test_find()
{
    // rf_object_t v = vector_i64(100000000);
    // for (int i = 0; i < 100000000; i++)
    // {
    //     as_vector_i64(&v)[i] = i;
    // }

    // clock_t start, end;
    // f64_t cpu_time_used;
    // start = clock();
    // i64_t i = vector_find(&v, i64(99999999));
    // end = clock();
    // cpu_time_used = ((f64_t)(end - start)) / CLOCKS_PER_SEC;
    // printf("I: %lld\n", i);
    // printf("Time: %f ms\n", cpu_time_used * 1000);
}

null_t test_string_match()
{
    debug("-- %d\n", string_match("brown", "br?*wn"));
    debug("-- %d\n", string_match("broasdfasdfwn", "br?*wn"));
    debug("-- %d\n", string_match("browmwn", "br?*wn"));
    debug("-- %d\n", string_match("brown", "[wertfb]rown"));
    debug("-- %d\n", string_match("brown", "[^wertf]rown"));
    debug("-- %d\n", string_match("bro[wn", "[^wertf]ro[[wn"));
    debug("-- %d\n", string_match("bro^wn", "[^wertf]ro^wn"));
    debug("-- %d\n", string_match("brown", "br[?*]wn"));

    return;
}

null_t test_vector()
{
    debug("testing vector");

    rf_object_t v = vector_i64(1);
    as_vector_i64(&v)[0] = 1;

    for (i32_t i = 0; i < 1000000; i++)
    {
        vector_push(&v, i64(i));
    }

    debug("testing vector done");

    rf_object_free(&v);
}

null_t test_allocate_and_free()
{
    u64_t size = 1024; // size of the memory block to allocate
    null_t *ptr = rf_malloc(size);
    assert(ptr != NULL);
    rf_free(ptr);
    printf("test_allocate_and_free passed\n");
}

null_t test_multiple_allocations()
{
    u64_t size = 1024;
    null_t *ptr1 = rf_malloc(size);
    null_t *ptr2 = rf_malloc(size);
    assert(ptr1 != NULL);
    assert(ptr2 != NULL);
    assert(ptr1 != ptr2);
    rf_free(ptr1);
    rf_free(ptr2);
    printf("test_multiple_allocations passed\n");
}

null_t test_allocation_after_free()
{
    u64_t size = 1024;
    null_t *ptr1 = rf_malloc(size);
    assert(ptr1 != NULL);
    rf_free(ptr1);

    null_t *ptr2 = rf_malloc(size);
    assert(ptr2 != NULL);

    // the second allocation should be able to use the block freed by the first allocation
    assert(ptr1 == ptr2);

    rf_free(ptr2);
    printf("test_allocation_after_free passed\n");
}

null_t test_out_of_memory()
{
    u64_t size = 1ull << 38;
    null_t *ptr = rf_malloc(size);
    assert(ptr == NULL);
    printf("test_out_of_memory passed\n");
}

null_t test_large_number_of_allocations()
{
    i64_t i, num_allocs = 10000000; // Large number of allocations
    u64_t size = 1024;
    null_t **ptrs = rf_malloc(num_allocs * sizeof(null_t *));
    for (i = 0; i < num_allocs; i++)
    {
        ptrs[i] = rf_malloc(size);
        assert(ptrs[i] != NULL);
    }
    // Free memory in reverse order
    for (i64_t i = num_allocs - 1; i >= 0; i--)
        rf_free(ptrs[i]);

    rf_free(ptrs);

    printf("test_large_number_of_allocations passed\n");
}

null_t test_varying_sizes()
{
    u64_t size = 16;       // Start size
    u64_t num_allocs = 10; // number of allocations
    null_t *ptrs[num_allocs];
    for (u64_t i = 0; i < num_allocs; i++)
    {
        ptrs[i] = rf_malloc(size << i); // double the size at each iteration
        assert(ptrs[i] != NULL);
    }
    // Free memory in reverse order
    for (int i = num_allocs - 1; i >= 0; i--)
    {
        rf_free(ptrs[i]);
    }
    printf("test_varying_sizes passed\n");
}

null_t test_alloc_free()
{
    null_t *ptr1 = rf_malloc(8 * 10000000);
    null_t *ptr2 = rf_malloc(8 * 10000000);

    rf_free(ptr1);
    rf_free(ptr2);
}

i32_t main()
{
    // runtime_init(0);
    rf_alloc_init();

    // test_allocate_and_free();
    // test_multiple_allocations();
    // test_allocation_after_free();
    // test_out_of_memory();
    // test_large_number_of_allocations();
    // test_varying_sizes();

    test_alloc_free();

    // printf("All tests passed!\n");

    // runtime_cleanup();

    rf_alloc_cleanup();
}
