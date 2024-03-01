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
#include "fs.h"
#include "string.h"
#include "util.h"
#include "heap.h"

#if defined(_WIN32) || defined(__CYGWIN__)

i64_t fs_fopen(str_p path, i64_t attrs)
{
    str_p tmp_path = str_dup(path);
    str_p p = tmp_path;
    str_p slash;

    while ((slash = strchr(p + 1, '/')) != NULL)
    {
        *slash = '\0';
        CreateDirectory(tmp_path, NULL);
        *slash = '/';
        p = slash;
    }

    heap_free(tmp_path);

    return (i64_t)CreateFile(path, attrs, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

i64_t fs_fsize(i64_t fd)
{
    LARGE_INTEGER size;
    if (!GetFileSizeEx((HANDLE)fd, &size))
        return -1;

    return (i64_t)size.QuadPart;
}

i64_t fs_fread(i64_t fd, str_p buf, i64_t size)
{
    DWORD bytesRead = 0;
    if (!ReadFile((HANDLE)fd, buf, size, &bytesRead, NULL))
        return -1;

    return (i64_t)bytesRead;
}

i64_t fs_fwrite(i64_t fd, str_p buf, i64_t size)
{
    DWORD bytesWritten = 0;
    if (!WriteFile((HANDLE)fd, buf, size, &bytesWritten, NULL))
        return -1;

    return (i64_t)bytesWritten;
}

i64_t fs_fclose(i64_t fd)
{
    return CloseHandle((HANDLE)fd);
}

i64_t fs_dcreate(str_p path)
{
    return CreateDirectory(path, NULL);
}

i64_t fs_dopen(str_p path)
{
    HANDLE hFind;
    WIN32_FIND_DATA FindFileData;

    hFind = FindFirstFile(path, &FindFileData);
    if (hFind == INVALID_HANDLE_VALUE)
    {
        if (!CreateDirectory(path, NULL))
            return (i64_t)INVALID_HANDLE_VALUE;

        hFind = FindFirstFile(
            path, &FindFileData);
        if (hFind == INVALID_HANDLE_VALUE)
            return (i64_t)INVALID_HANDLE_VALUE;
    }
    return (i64_t)hFind;
}

i64_t fs_dclose(i64_t fd)
{
    return FindClose((HANDLE)fd);
}

#else

i64_t fs_fopen(str_p path, i64_t attrs)
{
    str_p tmp_path = str_dup(path);
    str_p p = tmp_path;
    str_p slash;

    while ((slash = strchr(p + 1, '/')) != NULL)
    {
        *slash = '\0';
        fs_dcreate(tmp_path);
        *slash = '/';
        p = slash;
    }

    heap_free(tmp_path);

    return open(path, attrs, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
}

i64_t fs_fsize(i64_t fd)
{
    struct stat st;
    fstat(fd, &st);
    return st.st_size;
}

i64_t fs_fread(i64_t fd, str_p buf, i64_t size)
{
    i64_t c = 0;

    while ((c = read(fd, buf, size - c)) > 0)
        buf += c;

    if (c == -1)
        return c;

    *buf = '\0';

    return size;
}

i64_t fs_fwrite(i64_t fd, str_p buf, i64_t size)
{
    i64_t c = 0, l = size;
    while ((c = write(fd, buf, l)) > 0)
    {
        buf += c;
        l -= c;
    }

    if (c == -1)
        return c;

    return size;
}

i64_t fs_fclose(i64_t fd)
{
    return close(fd);
}

i64_t fs_dcreate(str_p path)
{
    struct stat st = {0};

    if (stat(path, &st) == -1)
    {
        if (mkdir(path, 0777) == -1)
            return -1;
    }

    return 0;
}

i64_t fs_dopen(str_p path)
{
    DIR *dir = opendir(path);

    if (!dir)
    {
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

i64_t fs_dclose(i64_t fd)
{
    return closedir((DIR *)fd);
}

#endif
