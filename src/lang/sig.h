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

#ifndef RAY_SIG_H
#define RAY_SIG_H

#include <rayforce.h>

/* Usage line of a builtin: its call shape with each argument's name, type
 * and position, e.g. "(take <collection: ...> <count: ...>)".  NULL for a
 * builtin without an entry. */
const char* ray_sig_usage(const char* name);

/* An error a builtin returned for a bad call is rewritten to end with the
 * builtin's usage line, so the caller sees which arguments are expected, of
 * which types and in which order.  Only type, length, rank and arity errors
 * are rewritten; any other result is returned as it is.  The message is
 * replaced in place and `result` is returned. */
ray_t* ray_sig_note(const char* name, ray_t* result);

/* The arity error of builtin `name` called with `got` arguments where `want`
 * (a count, or a range such as "2 to 4") are taken; carries the usage line. */
ray_t* ray_sig_arity(const char* name, const char* want, int got);

#endif
