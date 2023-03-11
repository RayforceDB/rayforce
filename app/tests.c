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
#include <pthread.h>
#include <string.h>
#include "../core/storm.h"
#include "../core/format.h"
#include "../core/monad.h"
#include "../core/alloc.h"
#include "../core/vm.h"
#include "../core/hash.h"
#include "../core/symbols.h"
#include "parse.h"
#include <time.h>
#include <stdlib.h>

// int test_hash()
// {
//     hash_table_t *table = ht_create();
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
    clock_t start, end;
    f64_t cpu_time_used;

    str_t str[1000000];

    u64_t pg_size = 4096 * 1024;

    u64_t *buckets = malloc(pg_size * sizeof(u64_t));
    memset(buckets, 0, pg_size * sizeof(u64_t));

    for (int i = 0; i < 1000000; i++)
    {
        str[i] = (str_t)malloc(10);
        snprintf(str[i], 10, "%d", 100000000 + i);
    }

    start = clock();

    for (int i = 0; i < 1000000; i++)
    {
        // printf("%s\n", str[i]);
        string_t s = string_create(str[i], strlen(str[i]));
        i64_t id = symbols_intern(s);
        // str_t val = symbols_get(id);
        // if (val == NULL)
        //     printf("NULL -- ID: %lld ORIG: %s\n", id, str[i]);
        // else
        //     printf("%s\n", val);
    }

    end = clock();

    cpu_time_used = ((f64_t)(end - start)) / CLOCKS_PER_SEC;
    printf("Time: %f ms\n", cpu_time_used * 1000);
    return 0;
}

int main()
{
    storm_alloc_init();

    test_symbols();

    storm_alloc_deinit();
}
