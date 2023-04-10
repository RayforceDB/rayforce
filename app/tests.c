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

    str_t st[1000000];

    i64_t pg_size = 4096 * 1024;

    i64_t *buckets = malloc(pg_size * sizeof(i64_t));
    memset(buckets, 0, pg_size * sizeof(i64_t));

    for (int i = 0; i < 1000000; i++)
    {
        st[i] = (str_t)malloc(10);
        snprintf(st[i], 10, "%d", 100000000 + i);
    }

    start = clock();

    for (int i = 0; i < 1000000; i++)
    {
        // printf("%s\n", st[i]);
        rf_object_t s = str(st[i], strlen(st[i]));
        i64_t id = symbols_intern(&s);
        // str_t val = symbols_get(id);
        // if (val == NULL)
        //     printf("NULL -- ID: %lld ORIG: %s\n", id, st[i]);
        // else
        //     printf("%s\n", val);
    }

    // rf_object_t s = str("code", 4);
    // i64_t id1 = symbols_intern(&s), id2;
    // str_t val = symbols_get(id1);

    // s = str("code", 4);
    // id2 = symbols_intern(&s);
    // val = symbols_get(id2);

    // printf("%d == %d\n", id1, id2);

    end = clock();

    cpu_time_used = ((f64_t)(end - start)) / CLOCKS_PER_SEC;
    printf("Time: %f ms\n", cpu_time_used * 1000);
    return 0;
}

null_t test_find()
{
    rf_object_t v = vector_i64(100000000);
    for (int i = 0; i < 100000000; i++)
    {
        as_vector_i64(&v)[i] = i;
    }

    clock_t start, end;
    f64_t cpu_time_used;
    start = clock();
    i64_t i = vector_find(&v, i64(99999999));
    end = clock();
    cpu_time_used = ((f64_t)(end - start)) / CLOCKS_PER_SEC;
    printf("I: %lld\n", i);
    printf("Time: %f ms\n", cpu_time_used * 1000);
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
typedef struct error_t
{
    char *file_path;
    int error_code;
    char *error_msg;
    int start_line;
    int end_line;
    int start_column;
    int end_column;
} error_t;

null_t test_span()
{
    error_t error;
    error.file_path = "./core/rayforce.h";
    error.error_code = 1;
    error.error_msg = "An example error message";
    error.start_line = 33;
    error.end_line = 35;
    error.start_column = 5;
    error.end_column = 10;

    pretty_print_error(&error);
}
i32_t main()
{
    runtime_init();

    // test_symbols();

    test_span();

    // test_string_match();
    runtime_cleanup();
}
