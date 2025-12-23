/*
 *   Copyright (c) 2025 Anton Kundenko <singaraiona@gmail.com>
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

#include "os.h"
#include "error.h"
#include "log.h"
#include "string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

i64_t os_get_var(lit_p name, c8_t buf[], i64_t len) {
    if (!name || !buf || len == 0) {
        LOG_ERROR("os_get_var: Invalid arguments");
        return -1;
    }

#ifdef _WIN32
    DWORD result = GetEnvironmentVariableA(name, buf, (DWORD)len);
    if (result == 0)
        return -1;
    if (result >= len) {
        LOG_ERROR("Buffer too small for environment variable '%s'", name);
        return -1;
    }
#else
    const char* value = getenv(name);
    if (!value)
        return -1;
    if (strlen(value) >= (u64_t)len) {
        LOG_ERROR("Buffer too small for environment variable '%s'", name);
        return -1;
    }
    strncpy(buf, value, len - 1);
    buf[len - 1] = '\0';
#endif

    return 0;
}

i64_t os_set_var(lit_p name, lit_p value) {
    if (!name || !value) {
        return -1;  // Invalid parameters
    }

#if defined(OS_WINDOWS)
    // Windows implementation using SetEnvironmentVariable
    if (!SetEnvironmentVariableA(name, value))
        return -3;
#else
    // UNIX implementation using setenv
    if (setenv(name, value, 1) != 0)
        return -3;

#endif

    return 0;  // Success
}

obj_p ray_os_get_var(obj_p x) {
    c8_t buf[1024];
    i64_t res;
    obj_p s;

    if (x->type != TYPE_C8)
        return ray_err("os-get-var: expected string");

    s = cstring_from_str(AS_C8(x), x->len);
    res = os_get_var(AS_C8(s), buf, sizeof(buf));
    drop_obj(s);

    if (res == -1)
        return ray_err("os-get-var: failed to get environment variable");

    return string_from_str(buf, strlen(buf));
}

obj_p ray_os_set_var(obj_p x, obj_p y) {
    i64_t res;
    obj_p sx, sy;

    if (x->type != TYPE_C8 || y->type != TYPE_C8)
        return ray_err("os-set-var: expected strings");

    sx = cstring_from_str(AS_C8(x), x->len);
    sy = cstring_from_str(AS_C8(y), y->len);
    res = os_set_var(AS_C8(sx), AS_C8(sy));
    drop_obj(sx);
    drop_obj(sy);

    if (res == -1)
        return ray_err("os-set-var: invalid arguments");
    if (res == -2)
        return ray_err("os-set-var: name or value too long");
    if (res == -3)
        return ray_err("os-set-var: failed to set environment variable");

    return NULL_OBJ;
}