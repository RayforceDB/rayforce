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

#ifndef DEF_H
#define DEF_H

#define RAYFORCE_MAJOR_VERSION 0
#define RAYFORCE_MINOR_VERSION 1
#define RAYFORCE_VERSION (RAYFORCE_MAJOR_VERSION >> 3 | RAYFORCE_MINOR_VERSION)

#if defined(_WIN32) || defined(__CYGWIN__)
#define OS_WINDOWS
#include <stdio.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <MSWSock.h>
#include <windows.h>
#include <direct.h>
#define MSG_NOSIGNAL 0
#define RAY_PAGE_SIZE 4096
#define MAP_FAILED (raw_p)(-1)
#define getcwd _getcwd
#elif defined(__linux__)
#define OS_LINUX
#define OS_UNIX
#define _POSIX_C_SOURCE 200809L  // Define POSIX source version for CLOCK_REALTIME
#ifndef __USE_MISC
#define __USE_MISC
#define _DEFAULT_SOURCE
#endif
#define RAY_PAGE_SIZE 4096
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>
#include <arpa/inet.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#elif defined(__APPLE__) && defined(__MACH__)
#define OS_MACOS
#define OS_UNIX
#define _DARWIN_C_SOURCE
#ifndef __USE_MISC
#define __USE_MISC
#define _DEFAULT_SOURCE
#endif
#if defined(__arm64__)
#define RAY_PAGE_SIZE 65536  // 64K for Apple Silicon
#else
#define RAY_PAGE_SIZE 16384  // 16K for Intel Mac
#endif
#include <stdio.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/mman.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include "dynlib.h"
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <mach/mach.h>
#include <mach/thread_policy.h>
#elif defined(__EMSCRIPTEN__)
#define OS_WASM
#define RAY_PAGE_SIZE 65536
#include <unistd.h>
#include <emscripten.h>
#else
#error "Unsupported platform"
#endif

#endif  // DEF_H
