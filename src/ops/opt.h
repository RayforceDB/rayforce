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

#ifndef RAY_OPT_H
#define RAY_OPT_H

#include "ops.h"

/* Un-staticed helpers exposed for use by idiom.c and future passes. */
ray_op_t*      graph_alloc_node_opt(ray_graph_t* g);
void           redirect_consumers(ray_graph_t* g, uint32_t old_id,
                                  ray_op_t* new_target,
                                  uint32_t skip_a, uint32_t skip_b);
ray_op_ext_t*  find_ext(ray_graph_t* g, uint32_t node_id);
/* Partition-key pruning bitmap for one comparison against a literal (see
 * opt.c); false when the comparison cannot prune. */
bool           ray_part_prune_mask(ray_t* key_values, uint16_t cmp_op,
                                   bool swapped, ray_t* lit, uint64_t* mask);

#endif /* RAY_OPT_H */
