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
#include "error.h"
#include "string.h"
#include "dynlib.h"
#include "heap.h"
#include "runtime.h"
#include "log.h"

#if defined(OS_WINDOWS)

obj_p dynlib_loadfn(str_p path, str_p func, i64_t nargs) {
    HMODULE handle;
    FARPROC dsym;
    obj_p fn;

    handle = LoadLibrary(path);
    if (!handle)
        THROW(ERR_SYS, "Failed to load shared library: %d", GetLastError());

    dsym = GetProcAddress(handle, func);
    if (!dsym)
        THROW(ERR_SYS, "Failed to load symbol from shared library: %d", GetLastError());

    switch (nargs) {
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

dynlib_p dynlib_open(obj_p path) {
    i64_t i, l;
    dynlib_p dl;
    HMODULE handle;
    obj_p dynlibs;

    // try to find the dynlib in the list (if it is already opened)
    dynlibs = runtime_get()->dynlibs;
    l = dynlibs->len;

    for (i = 0; i < l; i++) {
        dl = (dynlib_p)AS_I64(dynlibs)[i];
        if (str_cmp(AS_C8(dl->path), dl->path->len, AS_C8(path), path->len) == 0) {
            LOG_TRACE("dynlib: %s already opened", AS_C8(path));
            return dl;
        }
    }

    // otherwise, open the dynlib
    LOG_TRACE("dynlib: opening %s", AS_C8(path));
    handle = LoadLibrary(AS_C8(path));
    if (handle == NULL) {
        LOG_ERROR("dynlib: failed to open %s: %lu", AS_C8(path), GetLastError());
        return NULL;
    }

    dl = (dynlib_p)heap_mmap(sizeof(struct dynlib_t));
    dl->path = clone_obj(path);
    dl->handle = (raw_p)handle;

    // add the dynlib to the list
    push_raw(&runtime_get()->dynlibs, (raw_p)&dl);

    return dl;
}

nil_t dynlib_close(dynlib_p dl) {
    FreeLibrary((HMODULE)dl->handle);
    drop_obj(dl->path);
    heap_unmap(dl, sizeof(struct dynlib_t));
}

#else

#include <dlfcn.h>

dynlib_p dynlib_open(obj_p path) {
    i64_t i, l;
    dynlib_p dl;
    raw_p handle;
    obj_p dynlibs;

    // try to find the dynlib in the list (if it is already opened)
    dynlibs = runtime_get()->dynlibs;
    l = dynlibs->len;

    for (i = 0; i < l; i++) {
        dl = (dynlib_p)AS_I64(dynlibs)[i];
        if (str_cmp(AS_C8(dl->path), dl->path->len, AS_C8(path), path->len) == 0) {
            LOG_TRACE("dynlib: %s already opened", AS_C8(path));
            return dl;
        }
    }

    // otherwise, open the dynlib
    LOG_TRACE("dynlib: opening %s", AS_C8(path));
    handle = dlopen(AS_C8(path), RTLD_NOW | RTLD_GLOBAL);
    if (handle == NULL) {
        LOG_ERROR("dynlib: failed to open %s: %s", AS_C8(path), dlerror());
        return NULL;
    }

    dl = (dynlib_p)heap_mmap(sizeof(struct dynlib_t));
    dl->path = clone_obj(path);
    dl->handle = handle;

    // add the dynlib to the list
    push_raw(&runtime_get()->dynlibs, (raw_p)&dl);

    return dl;
}

nil_t dynlib_close(dynlib_p dl) {
    dlclose(dl->handle);
    drop_obj(dl->path);
    heap_unmap(dl, sizeof(struct dynlib_t));
}

obj_p dynlib_loadfn(obj_p path, obj_p func, i64_t nargs) {
    dynlib_p dl;
    raw_p handle;
    raw_p dsym;
    obj_p fn;

    dl = dynlib_open(path);
    if (dl == NULL)
        THROW(ERR_SYS, "Failed to open shared library: %s", dlerror());

    handle = dl->handle;

    dsym = dlsym(handle, AS_C8(func));
    if (dsym == NULL)
        THROW(ERR_SYS, "Failed to load symbol from shared library: %s", dlerror());

    switch (nargs) {
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

obj_p ray_loadfn(obj_p *args, i64_t n) {
    obj_p path, func, res;

    if (n != 3)
        THROW(ERR_ARITY, "Expected 3 arguments, got %llu", n);

    if (!args[0] || !args[1] || !args[2])
        THROW_S(ERR_TYPE, "Null is not a valid argument");

    if (args[0]->type != TYPE_C8)
        THROW(ERR_TYPE, "Expected 'string path, got %s", type_name(args[0]->type));

    if (args[1]->type != TYPE_C8)
        THROW(ERR_TYPE, "Expected 'string fname, got %s", type_name(args[1]->type));

    if (args[2]->type != -TYPE_I64)
        THROW(ERR_TYPE, "Expected 'i64 arguments, got %s", type_name(args[2]->type));

    path = cstring_from_str(AS_C8(args[0]), args[0]->len);
    func = cstring_from_str(AS_C8(args[1]), args[1]->len);

    res = dynlib_loadfn(path, func, args[2]->i64);

    drop_obj(path);
    drop_obj(func);

    return res;
}
