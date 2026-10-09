/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
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

#ifndef RAY_PART_H
#define RAY_PART_H

#include <rayforce.h>

/* Partitioned table */
ray_t*    ray_read_parted(const char* db_root, const char* table_name);

/* Drop every partition table ray_read_parted keeps for reuse across mounts
 * (see "Partition reuse across mounts" in part.c).  Runtime shutdown calls
 * it before the heap and symbol table go away. */
void      ray_parted_cache_clear(void);

/* Enumerate the table names under a parted db root — the splayed-table
 * subdirectories (those with a `.d` schema) of the first partition.
 * Returns a sorted RAY_SYM vector of names suitable for ray_read_parted,
 * or an error object (e.g. `io` when no partitions exist). */
ray_t*    ray_parted_tables(const char* db_root);

/* Fill missing tables across a parted db: for every table present in any
 * partition, write an empty copy — schema taken from the most recent
 * partition that has it — into every partition that lacks it.  Returns a
 * sorted RAY_SYM vector of the partition names that were filled (empty when
 * nothing needed fixing), or an error. */
ray_t*    ray_parted_fill(const char* db_root);

#endif /* RAY_PART_H */
