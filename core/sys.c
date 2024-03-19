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

#include "sys.h"
#include <stdio.h>
#include "string.h"
#include "format.h"
#include "runtime.h"

#if defined(_WIN32) || defined(__CYGWIN__)
#include <windows.h>
#elif defined(__APPLE__) && defined(__MACH__)
#include <sys/types.h>
#include <sys/sysctl.h>
#include <stdint.h>
#include <unistd.h>
#elif defined(__linux__)
#include <stdint.h>
#include <unistd.h>
#elif defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif

i32_t cpu_cores()
{
#if defined(_WIN32) || defined(__CYGWIN__)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors;
#elif defined(__unix__)
    return sysconf(_SC_NPROCESSORS_ONLN);
#else
    return 1;
#endif
}

sys_info_t sys_info(i32_t threads)
{
    sys_info_t info;

    info.major_version = RAYFORCE_MAJOR_VERSION;
    info.minor_version = RAYFORCE_MINOR_VERSION;
    strncpy(info.build_date, __DATE__, sizeof(info.build_date));
    info.build_date[strcspn(info.build_date, "\n")] = 0; // Remove the newline
    info.cores = cpu_cores();
    info.threads = (threads == 0) ? info.cores : threads;

#if defined(_WIN32) || defined(__CYGWIN__)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    snprintf(info.cpu, sizeof(info.cpu), "%lu", si.dwProcessorType);

    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    GlobalMemoryStatusEx(&memInfo);
    info.mem = (i32_t)(memInfo.ullTotalPhys / (1024 * 1024));

#elif defined(__linux__)
    FILE *cpuFile = fopen("/proc/cpuinfo", "r");
    c8_t line[256];

    while (fgets(line, sizeof(line), cpuFile))
    {
        if (strncmp(line, "model name", 10) == 0)
        {
            strncpy(info.cpu, strchr(line, ':') + 2, sizeof(info.cpu) - 1);
            info.cpu[strcspn(info.cpu, "\n")] = 0; // Remove the newline
            break;
        }
    }
    fclose(cpuFile);

    FILE *memFile = fopen("/proc/meminfo", "r");
    while (fgets(line, sizeof(line), memFile))
    {
        if (strncmp(line, "MemTotal:", 9) == 0)
        {
            i32_t totalKB;
            sscanf(strchr(line, ':') + 1, "%d", &totalKB);
            info.mem = totalKB / 1024;
            break;
        }
    }
    fclose(memFile);

#elif defined(__APPLE__) && defined(__MACH__)
    size_t len = sizeof(info.cpu);
    sysctlbyname("machdep.cpu.brand_string", &info.cpu, &len, NULL, 0);

    i64_t memSize;
    len = sizeof(memSize);
    sysctlbyname("hw.memsize", &memSize, &len, NULL, 0);
    info.mem = (i32_t)(memSize / (1024 * 1024));

#elif defined(__EMSCRIPTEN__)
    snprintf(info.cpu, sizeof(info.cpu), "WASM target");
    info.mem = 0;

#else
    snprintf(info.cpu, sizeof(info.cpu), "Unknown arch");
    info.mem = 0;

#endif

    return info;
}
