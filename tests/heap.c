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

test_result_t test_allocate_and_free()
{
    u64_t size = 1024; // size of the memory block to allocate
    nil_t *ptr = heap_alloc(size);
    TEST_ASSERT(ptr != NULL, "ptr != NULL");
    heap_free(ptr);

    PASS();
}

test_result_t test_multiple_allocations()
{
    u64_t size = 1024;
    nil_t *ptr1 = heap_alloc(size);
    nil_t *ptr2 = heap_alloc(size);
    TEST_ASSERT(ptr1 != NULL, "ptr1 != NULL");
    TEST_ASSERT(ptr2 != NULL, "ptr2 != NULL");
    TEST_ASSERT(ptr1 != ptr2, "ptr1 != ptr2");
    heap_free(ptr1);
    heap_free(ptr2);

    PASS();
}

test_result_t test_multiple_allocs_and_frees()
{
    u64_t i, size, n = 6;
    nil_t *ptrs[n];

    // Allocate multiple blocks of different sizes
    for (i = 0; i < n; i++)
    {
        size = i % 3;
        ptrs[i] = heap_alloc(size);
        TEST_ASSERT((size != 0 && ptrs[i] != NULL) || (size == 0 && ptrs[i] == NULL), "ptrs[i] != NULL");
    }

    // Free the allocated blocks in direct order
    for (i = 0; i < n; i++)
        heap_free(ptrs[i]);

    PASS();
}

test_result_t test_multiple_allocs_and_frees_rand()
{
    nil_t *ptrs[100];
    u64_t sizes[100];
    u64_t i, j, temp, num_allocs = 100;

    // Allocate multiple blocks of different sizes
    for (i = 0; i < num_allocs; i++)
    {
        sizes[i] = rand() % 1024; // Random size between 0 and 1023
        ptrs[i] = heap_alloc(sizes[i]);
        TEST_ASSERT(ptrs[i] != NULL, "ptrs[i] != NULL");
    }

    // Create an array of indices to shuffle the order of freeing
    u64_t indices[num_allocs];
    for (i = 0; i < num_allocs; i++)
        indices[i] = i;

    // Shuffle the indices array
    for (i = num_allocs - 1; i > 0; i--)
    {
        j = rand() % (i + 1);
        temp = indices[i];
        indices[i] = indices[j];
        indices[j] = temp;
    }

    // Free the allocated blocks in the shuffled order
    for (i = 0; i < num_allocs; i++)
    {
        int index = indices[i];
        heap_free(ptrs[index]);
        ptrs[index] = NULL;
    }

    // Verify that all pointers are set to NULL after freeing
    for (i = 0; i < num_allocs; i++)
    {
        TEST_ASSERT(ptrs[i] == NULL, "ptrs[i] == NULL");
    }

    PASS();
}

test_result_t test_realloc_larger_and_smaller()
{
    u64_t initial_size = 32;
    nil_t *ptr = heap_alloc(initial_size);
    TEST_ASSERT(ptr != NULL, "ptr != NULL");

    // Reallocate to a larger size
    u64_t larger_size = 128;
    nil_t *larger_ptr = heap_realloc(ptr, larger_size);
    TEST_ASSERT(larger_ptr != NULL, "larger_ptr != NULL");

    // Reallocate to a smaller size
    u64_t smaller_size = 16;
    nil_t *smaller_ptr = heap_realloc(larger_ptr, smaller_size);
    TEST_ASSERT(smaller_ptr != NULL, "smaller_ptr != NULL");

    heap_free(smaller_ptr);

    PASS();
}

test_result_t test_realloc_same_size()
{
    u64_t size = 64;
    nil_t *ptr = heap_alloc(size);
    TEST_ASSERT(ptr != NULL, "ptr != NULL");

    nil_t *new_ptr = heap_realloc(ptr, size);
    TEST_ASSERT(new_ptr != NULL, "new_ptr != NULL");
    TEST_ASSERT(new_ptr == ptr, "new_ptr == ptr");

    heap_free(new_ptr);

    PASS();
}

test_result_t test_alloc_dealloc_stress()
{
    u64_t i, j, n = 10000, m = 100, size;
    nil_t *ptrs[n];

    for (i = 0; i < n; i++)
    {
        size = rand() % 4096; // Random size between 0 and 4095
        ptrs[i] = heap_alloc(size);

        if (size == 0)
        {
            TEST_ASSERT(ptrs[i] == NULL, "ptrs[i] == NULL for size 0");
        }
        else
        {
            TEST_ASSERT(ptrs[i] != NULL, "ptrs[i] != NULL for size > 0");
        }

        if (i % m == 0)
        {
            for (j = 0; j < i; j++)
            {
                if (ptrs[j] != NULL)
                {
                    heap_free(ptrs[j]);
                    ptrs[j] = NULL;
                }
            }
        }
    }

    for (i = 0; i < n; i++)
    {
        if (ptrs[i] != NULL)
            heap_free(ptrs[i]);
    }

    PASS();
}

test_result_t test_allocation_after_free()
{
    u64_t size = 1024;
    nil_t *ptr1 = heap_alloc(size);
    TEST_ASSERT(ptr1 != NULL, "ptr1 != NULL");
    heap_free(ptr1);

    nil_t *ptr2 = heap_alloc(size);
    TEST_ASSERT(ptr2 != NULL, "ptr2 != NULL");

    // the second allocation should be able to use the block freed by the first allocation
    TEST_ASSERT(ptr1 == ptr2, "ptr1 == ptr2");

    heap_free(ptr2);

    PASS();
}

test_result_t test_out_of_memory()
{
    u64_t size = 1ull << 39;
    nil_t *ptr = heap_alloc(size);
    TEST_ASSERT(ptr == NULL, "ptr == NULL");

    PASS();
}

test_result_t test_varying_sizes()
{
    i64_t size = 16, i, num_allocs = 10;
    nil_t *ptrs[num_allocs];

    for (i = 0; i < num_allocs; i++)
    {
        ptrs[i] = heap_alloc(size << i); // double the size at each iteration
        TEST_ASSERT(ptrs[i] != NULL, "ptrs[i] != NULL");
    }
    // Free memory in reverse order
    for (i = num_allocs - 1; i >= 0; i--)
        heap_free(ptrs[i]);

    PASS();
}

test_result_t test_realloc()
{
    u64_t size = 13;
    nil_t *ptr = heap_alloc(size);
    TEST_ASSERT(ptr != NULL, "ptr != NULL");

    u64_t new_size = 47;
    nil_t *new_ptr = heap_realloc(ptr, new_size);
    TEST_ASSERT(new_ptr != NULL, "new_ptr != NULL");
    TEST_ASSERT(new_ptr != ptr, "new_ptr != ptr");

    heap_free(new_ptr);

    PASS();
}

test_result_t test_allocate_and_free_obj()
{
    // obj_p ht1 = vector_i64(12);
    // obj_p ht2 = vn_list(2, i64(1), i64(7));
    // obj_p ht3 = vector_i64(12);
    // obj_p ht4 = vn_list(2, i64(1), i64(7));
    obj_p ht5 = list(0);
    push_obj(&ht5, i64(345));
    // push_obj(&ht5, i64(145));

    // drop_obj(ht1);
    // drop_obj(ht2);
    // drop_obj(ht3);
    // drop_obj(ht4);
    drop_obj(ht5);

    PASS();
}