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

#ifndef FS_H
#define FS_H

// File attrs
#if defined(OS_WINDOWS)

#define ATTR_RDONLY GENERIC_READ
#define ATTR_WRONLY GENERIC_WRITE
#define ATTR_RDWR (ATTR_RDONLY | ATTR_WRONLY)
#define ATTR_CREAT 0
#define ATTR_TRUNC 0
#else
#include <stdint.h>
#include <unistd.h>
#include <dirent.h>
#define ATTR_RDONLY O_RDONLY
#define ATTR_WRONLY O_WRONLY
#define ATTR_RDWR O_RDWR
#define ATTR_CREAT O_CREAT
#define ATTR_TRUNC O_TRUNC
#endif
#include "rayforce.h"
#include <fcntl.h>

i64_t fs_fopen(lit_p path, i64_t attrs);
i64_t fs_fsize(i64_t fd);
i64_t fs_fread(i64_t fd, str_p buf, i64_t size);
i64_t fs_fwrite(i64_t fd, str_p buf, i64_t size);
i64_t fs_ftruncate(i64_t fd, i64_t size);
i64_t fs_fclose(i64_t fd);
i64_t fs_dcreate(lit_p path);
i64_t fs_dopen(lit_p path);
i64_t fs_dclose(i64_t fd);
obj_p fs_read_dir(lit_p path);

#endif // FS_H
