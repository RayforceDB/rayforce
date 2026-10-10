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

/* `rows` cells of `type`'s null on the heap: the sentinel ray_vec_set_null
 * writes, HAS_NULLS set; the non-nullable BOOL / U8 read 0; STR cells are
 * the empty string, GUID cells zero; SYM cells are id 0, the empty symbol,
 * at width W8 (id 0 fits it, and it is the width a ragged partition's
 * stand-in is encoded at, so its decode is this vector), resolved over
 * `dom` when given; LIST cells are the null object.  An owned vector, or an
 * error (`oom`; `nyi` for a type without a null fill). */
struct ray_sym_domain_s;
ray_t*    ray_null_fill(int8_t type, int64_t rows, struct ray_sym_domain_s* dom);

#endif /* RAY_PART_H */
