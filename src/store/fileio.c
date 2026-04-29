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

#include "fileio.h"

#include <limits.h>

/* PATH_MAX is mandated on POSIX (typically 4096 on Linux); Windows
 * caps at MAX_PATH = 260 unless long-path support is enabled.  Use the
 * larger of the two when known so callers passing deep splayed paths
 * (e.g. /db/yyyy.mm.dd/table/) don't silently truncate. */
#ifdef RAY_OS_WINDOWS
#  define RAY_PATH_MAX 4096
#elif defined(PATH_MAX)
#  define RAY_PATH_MAX PATH_MAX
#else
#  define RAY_PATH_MAX 4096
#endif

#ifdef RAY_OS_WINDOWS

#include <errno.h>

/* ===== Windows implementation ===== */

/* Translate GetLastError() into errno so callers can use errno portably. */
static void win_set_errno(void) {
    DWORD e = GetLastError();
    switch (e) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:    errno = ENOENT;  break;
    case ERROR_ACCESS_DENIED:     errno = EACCES;  break;
    case ERROR_WRITE_PROTECT:     errno = EROFS;   break;
    case ERROR_TOO_MANY_OPEN_FILES: errno = EMFILE; break;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:    errno = EEXIST;  break;
    default:                      errno = EIO;     break;
    }
}

ray_fd_t ray_file_open(const char* path, int flags) {
    if (!path) return RAY_FD_INVALID;

    DWORD access = 0;
    DWORD creation = OPEN_EXISTING;

    if (flags & RAY_OPEN_READ)  access |= GENERIC_READ;
    if (flags & RAY_OPEN_WRITE) access |= GENERIC_WRITE;
    if (flags & RAY_OPEN_CREATE) creation = OPEN_ALWAYS;

    HANDLE h = CreateFileA(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, creation, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) win_set_errno();
    return h;
}

void ray_file_close(ray_fd_t fd) {
    if (fd != RAY_FD_INVALID) CloseHandle(fd);
}

ray_err_t ray_file_lock_ex(ray_fd_t fd) {
    if (fd == RAY_FD_INVALID) return RAY_ERR_IO;
    OVERLAPPED ov = {0};
    if (!LockFileEx(fd, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &ov))
        return RAY_ERR_IO;
    return RAY_OK;
}

ray_err_t ray_file_lock_sh(ray_fd_t fd) {
    if (fd == RAY_FD_INVALID) return RAY_ERR_IO;
    OVERLAPPED ov = {0};
    if (!LockFileEx(fd, 0, 0, MAXDWORD, MAXDWORD, &ov))
        return RAY_ERR_IO;
    return RAY_OK;
}

ray_err_t ray_file_unlock(ray_fd_t fd) {
    if (fd == RAY_FD_INVALID) return RAY_OK;
    OVERLAPPED ov = {0};
    if (!UnlockFileEx(fd, 0, MAXDWORD, MAXDWORD, &ov))
        return RAY_ERR_IO;
    return RAY_OK;
}

ray_err_t ray_file_sync(ray_fd_t fd) {
    if (fd == RAY_FD_INVALID) return RAY_ERR_IO;
    if (!FlushFileBuffers(fd)) return RAY_ERR_IO;
    return RAY_OK;
}

ray_err_t ray_file_sync_dir(const char* path) {
    /* Windows: rename durability is handled by MOVEFILE_WRITE_THROUGH in
     * ray_file_rename; no separate directory fsync needed. */
    (void)path;
    return RAY_OK;
}

ray_err_t ray_file_rename(const char* old_path, const char* new_path) {
    if (!old_path || !new_path) return RAY_ERR_IO;
    /* MOVEFILE_WRITE_THROUGH flushes the rename to disk before returning,
     * providing crash-safe durability equivalent to POSIX fsync-after-rename. */
    if (!MoveFileExA(old_path, new_path,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return RAY_ERR_IO;
    return RAY_OK;
}

ray_err_t ray_mkdir(const char* path) {
    if (!path) return RAY_ERR_IO;
    if (!CreateDirectoryA(path, NULL)) {
        if (GetLastError() != ERROR_ALREADY_EXISTS) return RAY_ERR_IO;
    }
    return RAY_OK;
}

ray_err_t ray_mkdir_p(const char* path) {
    if (!path || !*path) return RAY_ERR_IO;
    char buf[RAY_PATH_MAX];
    size_t len = strlen(path);
    if (len >= sizeof(buf)) return RAY_ERR_IO;
    memcpy(buf, path, len + 1);
    /* Normalize trailing separator: trim it so the loop creates `buf` itself. */
    while (len > 1 && (buf[len - 1] == '/' || buf[len - 1] == '\\')) buf[--len] = '\0';
    for (size_t i = 1; i < len; i++) {
        if (buf[i] == '/' || buf[i] == '\\') {
            char saved = buf[i];
            buf[i] = '\0';
            if (!CreateDirectoryA(buf, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
                buf[i] = saved;
                return RAY_ERR_IO;
            }
            buf[i] = saved;
        }
    }
    if (!CreateDirectoryA(buf, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return RAY_ERR_IO;
    return RAY_OK;
}

#else

/* ===== POSIX implementation ===== */

#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

ray_fd_t ray_file_open(const char* path, int flags) {
    if (!path) return RAY_FD_INVALID;

    int oflags = 0;
    if ((flags & RAY_OPEN_READ) && (flags & RAY_OPEN_WRITE))
        oflags = O_RDWR;
    else if (flags & RAY_OPEN_WRITE)
        oflags = O_WRONLY;
    else
        oflags = O_RDONLY;

    if (flags & RAY_OPEN_CREATE) oflags |= O_CREAT;

    return open(path, oflags, 0644);
}

void ray_file_close(ray_fd_t fd) {
    if (fd != RAY_FD_INVALID) close(fd);
}

ray_err_t ray_file_lock_ex(ray_fd_t fd) {
    if (fd == RAY_FD_INVALID) return RAY_ERR_IO;
    if (flock(fd, LOCK_EX) != 0) return RAY_ERR_IO;
    return RAY_OK;
}

ray_err_t ray_file_lock_sh(ray_fd_t fd) {
    if (fd == RAY_FD_INVALID) return RAY_ERR_IO;
    if (flock(fd, LOCK_SH) != 0) return RAY_ERR_IO;
    return RAY_OK;
}

ray_err_t ray_file_unlock(ray_fd_t fd) {
    if (fd == RAY_FD_INVALID) return RAY_OK;
    if (flock(fd, LOCK_UN) != 0) return RAY_ERR_IO;
    return RAY_OK;
}

ray_err_t ray_file_sync(ray_fd_t fd) {
    if (fd == RAY_FD_INVALID) return RAY_ERR_IO;
    if (fsync(fd) != 0) return RAY_ERR_IO;
    return RAY_OK;
}

ray_err_t ray_file_sync_dir(const char* path) {
    if (!path) return RAY_ERR_IO;
    /* Extract parent directory from path */
    char dir[1024];
    size_t len = strlen(path);
    if (len >= sizeof(dir)) return RAY_ERR_IO;
    memcpy(dir, path, len + 1);
    /* Find last '/' */
    char* slash = strrchr(dir, '/');
    if (slash) {
        if (slash == dir)
            dir[1] = '\0';  /* root directory */
        else
            *slash = '\0';
    } else {
        dir[0] = '.'; dir[1] = '\0';  /* current directory */
    }
    int fd = open(dir, O_RDONLY);
    if (fd < 0) return RAY_ERR_IO;
    int rc = fsync(fd);
    close(fd);
    return (rc == 0) ? RAY_OK : RAY_ERR_IO;
}

ray_err_t ray_file_rename(const char* old_path, const char* new_path) {
    if (!old_path || !new_path) return RAY_ERR_IO;
    if (rename(old_path, new_path) != 0) return RAY_ERR_IO;
    return RAY_OK;
}

ray_err_t ray_mkdir(const char* path) {
    if (!path) return RAY_ERR_IO;
    if (mkdir(path, 0755) != 0 && errno != EEXIST) return RAY_ERR_IO;
    return RAY_OK;
}

ray_err_t ray_mkdir_p(const char* path) {
    if (!path || !*path) return RAY_ERR_IO;
    char buf[RAY_PATH_MAX];
    size_t len = strlen(path);
    if (len >= sizeof(buf)) return RAY_ERR_IO;
    memcpy(buf, path, len + 1);
    /* Strip trailing slash so the final mkdir creates `buf` itself. */
    while (len > 1 && buf[len - 1] == '/') buf[--len] = '\0';
    for (size_t i = 1; i < len; i++) {
        if (buf[i] == '/') {
            buf[i] = '\0';
            if (mkdir(buf, 0755) != 0 && errno != EEXIST) return RAY_ERR_IO;
            buf[i] = '/';
        }
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST) return RAY_ERR_IO;
    return RAY_OK;
}

#endif
