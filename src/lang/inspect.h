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

#ifndef RAY_LANG_INSPECT_H
#define RAY_LANG_INSPECT_H

#include <rayforce.h>

/* Read-only metadata of the live session for editor tooling (#947, #946,
 * #945): each takes an explicitly named global and answers from what the
 * engine already holds, evaluating and calling nothing. */

/* The value a name holds, read without evaluating: the global of that exact
 * name (dotted builtin names included), else, for a dotted name, the
 * dictionary members along its segments from its global head.  Never a
 * table column, a call or a method.  Owned; NULL when unbound. */
ray_t* ray_inspect_lookup(int64_t sym_id);

/* (.repl.signature name): callable kind, calling convention, special form,
 * lambda parameters, documented usage. */
ray_t* ray_repl_signature_fn(ray_t* name);

/* (.repl.definition name): where the function a name holds now was
 * written, from the source record its lambda already retains. */
ray_t* ray_repl_definition_fn(ray_t* name);

#endif
