/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

#ifndef RAY_FILEIO_H
#define RAY_FILEIO_H

#include <rayforce.h>

/* Cross-platform file I/O (locking, sync, atomic rename) */
#ifdef RAY_OS_WINDOWS
  #include <windows.h>
  typedef HANDLE ray_fd_t;
  #define RAY_FD_INVALID INVALID_HANDLE_VALUE
#else
  typedef int ray_fd_t;
  #define RAY_FD_INVALID (-1)
#endif

#define RAY_OPEN_READ   0x01
#define RAY_OPEN_WRITE  0x02
#define RAY_OPEN_CREATE 0x04

ray_fd_t  ray_file_open(const char* path, int flags);
void     ray_file_close(ray_fd_t fd);
ray_err_t ray_file_lock_ex(ray_fd_t fd);
ray_err_t ray_file_lock_sh(ray_fd_t fd);
ray_err_t ray_file_unlock(ray_fd_t fd);
ray_err_t ray_file_sync(ray_fd_t fd);
ray_err_t ray_file_sync_dir(const char* path);
ray_err_t ray_file_rename(const char* old_path, const char* new_path);
ray_err_t ray_mkdir(const char* path);
ray_err_t ray_mkdir_p(const char* path);  /* like `mkdir -p` */

#endif /* RAY_FILEIO_H */
