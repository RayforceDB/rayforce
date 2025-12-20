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

#include <stdio.h>
#include <sys/stat.h>
#include "log.h"
#if defined(OS_WINDOWS)
#include <io.h>  // for _get_osfhandle
#endif
#include "fs.h"
#include "string.h"
#include "ops.h"  // for NULL_OBJ

#if defined(OS_WINDOWS)

i64_t fs_fopen(lit_p path, i64_t attrs) {
    obj_p s;
    str_p tmp_path, p, slash;
    DWORD flags = FILE_ATTRIBUTE_NORMAL;
    DWORD disposition = OPEN_ALWAYS;

    s = cstring_from_str(path, strlen(path));
    tmp_path = AS_C8(s);
    p = tmp_path;

    while ((slash = strchr(p + 1, '/')) != NULL) {
        *slash = '\0';
        CreateDirectory(tmp_path, NULL);
        *slash = '/';
        p = slash;
    }

    drop_obj(s);

    // Handle append flag
    if (attrs & ATTR_APPEND) {
        flags |= FILE_APPEND_DATA;
    }

    return (i64_t)CreateFile(path, attrs, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, disposition, flags, NULL);
}

i64_t fs_fdelete(lit_p path) {
    i64_t res;
    obj_p s;
    s = cstring_from_str(path, strlen(path));
    res = DeleteFile(AS_C8(s));
    drop_obj(s);

    return res;
}

i64_t fs_fsize(i64_t fd) {
    LARGE_INTEGER size;
    if (!GetFileSizeEx((HANDLE)fd, &size))
        return -1;

    return (i64_t)size.QuadPart;
}

i64_t fs_fread(i64_t fd, str_p buf, i64_t size) {
    DWORD bytesRead = 0;
    if (!ReadFile((HANDLE)fd, buf, size, &bytesRead, NULL))
        return -1;

    return (i64_t)bytesRead;
}

i64_t fs_fwrite(i64_t fd, str_p buf, i64_t size) {
    DWORD bytesWritten = 0;
    if (!WriteFile((HANDLE)fd, buf, size, &bytesWritten, NULL))
        return -1;

    return (i64_t)bytesWritten;
}

i64_t fs_file_extend(i64_t fd, i64_t size) {
    HANDLE hFile = (HANDLE)fd;
    LARGE_INTEGER li;
    li.QuadPart = size;

    // Move the file pointer to the desired position
    if (!SetFilePointerEx(hFile, li, NULL, FILE_BEGIN))
        return -1;

    // Set the end of the file to the current position of the file pointer
    if (!SetEndOfFile(hFile))
        return -1;

    return size;
}

i64_t fs_fclose(i64_t fd) { return CloseHandle((HANDLE)fd); }

i64_t fs_dcreate(lit_p path) { return CreateDirectory(path, NULL); }

i64_t fs_dopen(lit_p path) {
    HANDLE hFind;
    WIN32_FIND_DATA FindFileData;

    hFind = FindFirstFile(path, &FindFileData);
    if (hFind == INVALID_HANDLE_VALUE) {
        if (!CreateDirectory(path, NULL))
            return (i64_t)INVALID_HANDLE_VALUE;

        hFind = FindFirstFile(path, &FindFileData);
        if (hFind == INVALID_HANDLE_VALUE)
            return (i64_t)INVALID_HANDLE_VALUE;
    }
    return (i64_t)hFind;
}

i64_t fs_dclose(i64_t fd) { return FindClose((HANDLE)fd); }

obj_p fs_read_dir(lit_p path) {
    WIN32_FIND_DATA findFileData;
    HANDLE hFind;
    obj_p lst = LIST(0);
    char searchPath[MAX_PATH];

    // Append /* to the path (Windows accepts forward slashes)
    snprintf(searchPath, MAX_PATH, "%s/*", path);

    hFind = FindFirstFile(searchPath, &findFileData);
    if (hFind == INVALID_HANDLE_VALUE)
        return sys_error(ERROR_TYPE_SYS, path);

    do {
        // Skip . and .. entries
        if (strcmp(findFileData.cFileName, ".") == 0 || strcmp(findFileData.cFileName, "..") == 0)
            continue;
        push_obj(&lst, string_from_str(findFileData.cFileName, strlen(findFileData.cFileName)));
    } while (FindNextFile(hFind, &findFileData) != 0);

    FindClose(hFind);

    return lst;
}

i64_t fs_get_fname_by_fd(i64_t fd, c8_t buf[], i64_t len) {
    HANDLE hFile = (HANDLE)_get_osfhandle(fd);  // Convert fd to Windows HANDLE
    if (hFile == INVALID_HANDLE_VALUE) {
        LOG_ERROR("Invalid file descriptor");
        return -1;
    }

    DWORD result = GetFinalPathNameByHandleA(hFile, buf, (DWORD)len, FILE_NAME_NORMALIZED);
    if (result == 0 || result >= len) {
        LOG_ERROR("Failed to get file name or buffer too small: %lu", GetLastError());
        return -1;
    }

    return 0;
}

#elif defined(OS_WASM)

// clang-format off
EM_JS(nil_t, fs_dcreate_js, (lit_p path), {
    FS.mkdir(UTF8ToC8(path));
});

EM_JS(i64_t, fs_dopen_js, (lit_p path), {
    try
    {
        FS.mkdir(UTF8ToC8(path));
    }
    catch (e)
    {
        if (e.code !== 'EEXIST')
            throw e; // Ignore 'EEXIST' and throw other errors
    }
    return Module._opendir(UTF8ToC8(path));
});
// clang-format on

i64_t fs_fopen(lit_p path, i64_t attrs) {
    return open(path, attrs, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
}

i64_t fs_fdelete(lit_p path) {
    i64_t res;
    obj_p s;
    s = cstring_from_str(path, strlen(path));
    res = unlink(AS_C8(s));
    drop_obj(s);

    return res;
}

i64_t fs_fsize(i64_t fd) {
    struct stat st;
    if (fstat(fd, &st) == -1)
        return -1;

    return st.st_size;
}

i64_t fs_fread(i64_t fd, str_p buf, i64_t size) {
    i64_t bytesRead, totalRead = 0;

    while (totalRead < size && (bytesRead = read(fd, buf + totalRead, size - totalRead)) > 0)
        totalRead += bytesRead;

    if (bytesRead == -1)
        return -1;  // Read error

    buf[totalRead] = '\0';  // Null terminate the string

    return totalRead;
}

i64_t fs_fwrite(i64_t fd, str_p buf, i64_t size) {
    i64_t bytesWritten, totalWritten = 0;

    while (totalWritten < size && (bytesWritten = write(fd, buf + totalWritten, size - totalWritten)) > 0)
        totalWritten += bytesWritten;

    if (bytesWritten == -1)
        return -1;  // Write error

    return totalWritten;
}

i64_t fs_fclose(i64_t fd) { return close(fd); }

i64_t fs_dcreate(lit_p path) {
    fs_dcreate_js(path);

    return 0;
}

i64_t fs_dopen(lit_p path) { return fs_dopen_js(path); }

i64_t fs_dclose(i64_t fd) { return closedir((DIR *)fd); }

i64_t fs_file_extend(i64_t fd, i64_t size) {
    if (lseek(fd, size - 1, SEEK_SET) == -1)
        return -1;

    if (write(fd, "", 1) == -1)
        return -1;

    return size;
}

obj_p fs_read_dir(lit_p path) {
    DIR *dir;
    struct dirent *ent;
    obj_p lst = LIST(0);

    if ((dir = opendir(path)) != NULL) {
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                continue;

            push_obj(&lst, string_from_str(ent->d_name, strlen(ent->d_name)));
        }

        closedir(dir);
    }

    return lst;
}

i64_t fs_get_fname_by_fd(i64_t fd, c8_t buf[], i64_t len) {
    (void)fd;
    (void)buf;
    (void)len;
    return -1;
}

#else

i64_t fs_fopen(lit_p path, i64_t attrs) {
    obj_p s;
    str_p tmp_path, p, slash;

    s = cstring_from_str(path, strlen(path));
    tmp_path = AS_C8(s);
    p = tmp_path;

    while ((slash = strchr(p + 1, '/')) != NULL) {
        *slash = '\0';
        fs_dcreate(tmp_path);
        *slash = '/';
        p = slash;
    }

    drop_obj(s);

    return open(path, attrs, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
}

i64_t fs_fdelete(lit_p path) {
    i64_t res;
    obj_p s;
    s = cstring_from_str(path, strlen(path));
    res = unlink(AS_C8(s));
    drop_obj(s);

    return res;
}

i64_t fs_fsize(i64_t fd) {
    struct stat st;
    if (fstat(fd, &st) == -1)
        return -1;

    return st.st_size;
}

i64_t fs_fread(i64_t fd, str_p buf, i64_t size) {
    i64_t c = 0;

    while ((c = read(fd, buf, size - c)) > 0)
        buf += c;

    if (c == -1)
        return c;

    *buf = '\0';

    return size;
}

i64_t fs_fwrite(i64_t fd, str_p buf, i64_t size) {
    i64_t c = 0, l = size;
    while ((c = write(fd, buf, l)) > 0) {
        buf += c;
        l -= c;
    }

    if (c == -1)
        return c;

    return size;
}

i64_t fs_file_extend(i64_t fd, i64_t size) {
    if (lseek(fd, size - 1, SEEK_SET) == -1)
        return -1;

    if (write(fd, "", 1) == -1)
        return -1;

    return size;
}

i64_t fs_fclose(i64_t fd) { return close(fd); }

i64_t fs_dcreate(lit_p path) {
#ifdef __cplusplus
    struct stat st{};
#else
    struct stat st = {0};
#endif

    if (stat(path, &st) == -1) {
        if (mkdir(path, 0777) == -1)
            return -1;
    }

    return 0;
}

i64_t fs_dopen(lit_p path) {
    DIR *dir = opendir(path);

    if (!dir) {
        // Try to create the directory if it doesn't exist
        if (mkdir(path, 0777) == -1)
            return -1;

        // Open the newly created directory
        dir = opendir(path);
        if (!dir)
            return -1;

        return (i64_t)dir;
    }

    return (i64_t)dir;
}

i64_t fs_dclose(i64_t fd) { return closedir((DIR *)fd); }

obj_p fs_read_dir(lit_p path) {
    DIR *dir;
    struct dirent *ent;
    obj_p lst = LIST(0);

    if ((dir = opendir(path)) != NULL) {
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                continue;

            push_obj(&lst, string_from_str(ent->d_name, strlen(ent->d_name)));
        }

        closedir(dir);
    }

    return lst;
}

i64_t fs_get_fname_by_fd(i64_t fd, c8_t buf[], i64_t len) {
    i64_t l;
    c8_t path[PATH_MAX];

    snprintf(path, sizeof(path), "/proc/self/fd/%lld", fd);

    // Read symbolic link - silently fail if fd is invalid (cleanup scenario)
    l = readlink(path, buf, len - 1);
    if (l == -1)
        return -1;

    buf[l] = '\0';  // Null-terminate the string

    return 0;
}

#endif

i64_t fs_filename(lit_p path, lit_p *name) {
    i64_t i, len;
    lit_p p;

    *name = NULL;
    len = strlen(path);

    // Handle empty string case
    if (len == 0)
        return 0;

    // Skip trailing slashes
    while (len > 0 && (path[len - 1] == '/'))
        len--;
    // If the entire path was slashes, return the original path
    if (len == 0)
        return 0;

    // Manually find the last occurrence of either '/'
    p = NULL;
    for (i = 0; i < len; i++) {
        if (path[i] == '/')
            p = &path[i];
    }

    *name = p ? p + 1 : path;

    return len - (*name - path);
}