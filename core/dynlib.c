/*
 *   Copyright (c) 2024 Anton Kundenko <singaraiona@gmail.com>
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
#include "util.h"
#include "error.h"

#if defined(OS_WINDOWS)

obj_p dynlib_loadfn(str_p path, str_p func, i64_t nargs)
{
    HMODULE handle;
    FARPROC dsym;
    obj_p fn;

    handle = LoadLibrary(path);
    if (!handle)
        throw(ERR_SYS, "Failed to load shared library: %d", GetLastError());

    dsym = GetProcAddress(handle, func);
    if (!dsym)
        throw(ERR_SYS, "Failed to load symbol from shared library: %d", GetLastError());

    switch (nargs)
    {
    case 1:
        fn = atom(-TYPE_UNARY);
        break;
    case 2:
        fn = atom(-TYPE_BINARY);
        break;
    default:
        fn = atom(-TYPE_VARY);
        break;
    }

    fn->i64 = (i64_t)dsym;
    fn->attrs = FN_NONE;

    return fn;
}

#else

obj_p dynlib_loadfn(str_p path, str_p func, i64_t nargs)
{
    raw_p handle, dsym;
    obj_p fn;
    str_p error;

    handle = dlopen(path, RTLD_NOW);
    if (!handle)
        throw(ERR_SYS, "Failed to load shared library: %s", dlerror());

    dsym = dlsym(handle, func);
    if ((error = dlerror()) != NULL)
        throw(ERR_SYS, "Failed to load symbol from shared library: %s", error);

    switch (nargs)
    {
    case 1:
        fn = atom(-TYPE_UNARY);
        break;
    case 2:
        fn = atom(-TYPE_BINARY);
        break;
    default:
        fn = atom(-TYPE_VARY);
        break;
    }

    fn->i64 = (i64_t)dsym;
    fn->attrs = FN_NONE;

    return fn;
}

#endif

obj_p ray_loadfn(obj_p *args, u64_t n)
{
    if (n != 3)
        throw(ERR_ARITY, "Expected 3 arguments, got %llu", n);

    if (!args[0] || !args[1] || !args[2])
        throw(ERR_TYPE, "Null is not a valid argument");

    if (args[0]->type != TYPE_C8)
        throw(ERR_TYPE, "Expected 'string path, got %s", type_name(args[0]->type));

    if (args[1]->type != TYPE_C8)
        throw(ERR_TYPE, "Expected 'string fname, got %s", type_name(args[1]->type));

    if (args[2]->type != -TYPE_I64)
        throw(ERR_TYPE, "Expected 'i64 arguments, got %s", type_name(args[2]->type));

    return dynlib_loadfn(as_string(args[0]), as_string(args[1]), args[2]->i64);
}
