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
#include "error.h"
#include "ipc.h"

#if defined(OS_WINDOWS)
#include <windows.h>
#define popen _popen
#define pclose _pclose
#define WIFEXITED(status) 1
#define WEXITSTATUS(status) (status)
#else
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#endif

i32_t cpu_cores() {
#if defined(OS_WASM)
    return 1;
#elif defined(OS_WINDOWS)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors;
#elif defined(OS_UNIX)
    return sysconf(_SC_NPROCESSORS_ONLN);
#else
    return 1;
#endif
}

sys_info_t sys_info(i32_t threads) {
    sys_info_t info;

    info.major_version = RAYFORCE_MAJOR_VERSION;
    info.minor_version = RAYFORCE_MINOR_VERSION;
    strncpy(info.build_date, __DATE__, sizeof(info.build_date));
    info.build_date[strcspn(info.build_date, "\n")] = 0;  // Remove the newline
    info.cores = cpu_cores();
    info.threads = (threads == 0 || threads > info.cores) ? info.cores : threads;
    strncpy(info.cpu, "Unknown CPU", sizeof(info.cpu));
    if (getcwd(info.cwd, sizeof(info.cwd)) == NULL)
        printf("Unable to get current working directory\n");

#if defined(OS_WINDOWS)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    snprintf(info.cpu, sizeof(info.cpu), "%lu", si.dwProcessorType);

    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    GlobalMemoryStatusEx(&memInfo);
    info.mem = (i32_t)(memInfo.ullTotalPhys / (1024 * 1024));

#elif defined(OS_LINUX)
    FILE *cpuFile = fopen("/proc/cpuinfo", "r");
    c8_t line[256];

    while (fgets(line, sizeof(line), cpuFile)) {
        if (strncmp(line, "model name", 10) == 0) {
            strncpy(info.cpu, strchr(line, ':') + 2, sizeof(info.cpu) - 1);
            info.cpu[strcspn(info.cpu, "\n")] = 0;  // Remove the newline
            break;
        }
    }

    fclose(cpuFile);

    FILE *memFile = fopen("/proc/meminfo", "r");
    while (fgets(line, sizeof(line), memFile)) {
        if (strncmp(line, "MemTotal:", 9) == 0) {
            i32_t totalKB;
            sscanf(strchr(line, ':') + 1, "%d", &totalKB);
            info.mem = totalKB / 1024;
            break;
        }
    }
    fclose(memFile);

#elif defined(OS_MACOS)
    size_t len = sizeof(info.cpu);
    sysctlbyname("machdep.cpu.brand_string", &info.cpu, &len, NULL, 0);

    i64_t memSize;
    len = sizeof(memSize);
    sysctlbyname("hw.memsize", &memSize, &len, NULL, 0);
    info.mem = (i32_t)(memSize / (1024 * 1024));

#elif defined(OS_WASM)
    snprintf(info.cpu, sizeof(info.cpu), "WASM target");
    info.mem = 0;

#else
    snprintf(info.cpu, sizeof(info.cpu), "Unknown arch");
    info.mem = 0;

#endif

    return info;
}

typedef struct command_entry_t {
    str_p name;
    obj_p (*func)(i32_t argc, str_p argv[]);
} command_entry_t;

#define COMMAND(name, func) {name, func}

// Internal commands list
command_entry_t commands[] = {
    COMMAND("use-unicode", sys_use_unicode),
    COMMAND("set-fpr", sys_set_fpr),
    COMMAND("set-display-width", sys_set_display_width),
    COMMAND("timeit-activate", sys_timeit_activate),
    COMMAND("listen", sys_listen),
    COMMAND("exit", sys_exit),
};

obj_p sys_set_fpr(i32_t argc, str_p argv[]) {
    i64_t fpr, res;

    if (argc != 1)
        THROW(ERR_LENGTH, "set-fpr: expected 1 argument");

    fpr = i64_from_str(argv[0], strlen(argv[0]));
    if (fpr < 0)
        THROW(ERR_LENGTH, "set-fpr: expected a positive integer");

    res = format_set_fpr(fpr);
    if (res != 0)
        THROW(ERR_LENGTH, "set-fpr: failed to set fpr");

    return i64(res);
}

obj_p sys_use_unicode(i32_t argc, str_p argv[]) {
    i64_t res;

    if (argc != 1)
        THROW(ERR_LENGTH, "use-unicode: expected 1 argument");

    res = format_set_use_unicode(i64_from_str(argv[0], strlen(argv[0])));
    if (res != 0)
        THROW(ERR_LENGTH, "use-unicode: failed to set use unicode");

    return i64(res);
}

obj_p sys_set_display_width(i32_t argc, str_p argv[]) {
    i64_t width, res;

    if (argc != 1)
        THROW(ERR_LENGTH, "set-display-width: expected 1 argument");

    width = i64_from_str(argv[0], strlen(argv[0]));
    if (width < 0)
        THROW(ERR_LENGTH, "set-display-width: expected a positive integer");

    res = format_set_display_width(width);
    if (res != 0)
        THROW(ERR_LENGTH, "set-display-width: failed to set display width");

    return i64(res);
}

obj_p sys_timeit_activate(i32_t argc, str_p argv[]) {
    if (argc != 1)
        THROW(ERR_LENGTH, "timeit-activate: expected 1 argument");

    timeit_activate(i64_from_str(argv[0], strlen(argv[0])));

    return i64(0);
}

obj_p sys_listen(i32_t argc, str_p argv[]) {
    UNUSED(argc);
    UNUSED(argv);

    i64_t res = 0;

    if (argc != 1)
        THROW(ERR_LENGTH, "listen: expected 1 argument");

    res = i64_from_str(argv[0], strlen(argv[0]));
    if (res < 0)
        THROW(ERR_TYPE, "listen: expected integer");

    res = ipc_listen(runtime_get()->poll, res);

    if (res == -1)
        return sys_error(ERROR_TYPE_SOCK, "listen");

    if (res == -2)
        THROW(ERR_LENGTH, "listen: already listening");

    return i64(res);
}

obj_p sys_exit(i32_t argc, str_p argv[]) {
    i64_t code;

    if (argc == 0)
        code = 0;
    else
        code = i64_from_str(argv[0], strlen(argv[0]));

    poll_exit(runtime_get()->poll, code);

    stack_push(NULL_OBJ);

    longjmp(ctx_get()->jmp, 2);

    __builtin_unreachable();
}

obj_p ray_internal_command(obj_p cmd) {
    str_p argv[64];  // Max 64 arguments
    c8_t *cmd_str, *current, cmd_buf[4096];
    i32_t i, cmd_len, remaining_len, argc;

    current = AS_C8(cmd);
    cmd_len = cmd->len;
    remaining_len = cmd_len;
    argc = 0;

    // Make a copy of the command string
    strncpy(cmd_buf, current, sizeof(cmd_buf) - 1);
    cmd_buf[sizeof(cmd_buf) - 1] = '\0';

    cmd_str = cmd_buf;
    current = cmd_buf;  // Use the copy for parsing

    // Find first space
    for (i = 0; i < cmd_len && current[i] != ' '; i++)
        ;

    if (i < cmd_len) {
        // We found a space, split command and arguments
        cmd_buf[i] = '\0';  // Null terminate command part
        remaining_len = cmd_len - i - 1;
        current = cmd_buf + i + 1;  // Use the copy for arguments

        // Parse arguments
        while (remaining_len > 0 && argc < 64) {
            // Skip leading spaces
            while (remaining_len > 0 && *current == ' ') {
                current++;
                remaining_len--;
            }
            if (remaining_len <= 0)
                break;

            // Find end of argument
            if (*current == '"') {
                // Handle quoted arguments
                current++;  // Skip opening quote
                remaining_len--;

                for (i = 0; i < remaining_len && current[i] != '"'; i++)
                    ;
                if (i >= remaining_len) {
                    THROW(ERR_PARSE, "unmatched quote in command arguments");
                }

                // Null terminate the argument
                current[i] = '\0';
                argv[argc++] = current;

                // Move past the closing quote and any following space
                current += i + 1;
                remaining_len -= i + 1;
                if (remaining_len > 0 && *current == ' ') {
                    current++;
                    remaining_len--;
                }
            } else {
                // Handle unquoted arguments
                for (i = 0; i < remaining_len && current[i] != ' '; i++)
                    ;

                // Null terminate the argument
                if (i < remaining_len) {
                    current[i] = '\0';
                    argv[argc++] = current;
                    current += i + 1;
                    remaining_len -= i + 1;
                } else {
                    // Ensure the last argument is null-terminated
                    current[remaining_len] = '\0';
                    argv[argc++] = current;
                    remaining_len = 0;
                }
            }
        }
    }

    // Find and execute the command
    for (i = 0; i < (i32_t)sizeof(commands) / (i32_t)sizeof(commands[0]); i++) {
        if (strcmp(cmd_str, commands[i].name) == 0) {
            return commands[i].func(argc, argv);
        }
    }

    return NULL_OBJ;
}

obj_p ray_system(obj_p cmd) {
    c8_t buf[4096];
    FILE *fp;
    i64_t l, status;
    obj_p c, res;

    if (cmd->type != TYPE_C8)
        THROW(ERR_TYPE, "system: expected a string");

    // Try internal command first
    res = ray_internal_command(cmd);
    if (res != NULL_OBJ)
        return res;

    // If not an internal command, execute as external command
    c = str_fmt(-1, "%.*s 2>&1", (i32_t)cmd->len, (lit_p)AS_U8(cmd));

    fp = popen(AS_C8(c), "r");
    if (fp == NULL)
        THROW(ERR_SYS, "popen failed");

    res = LIST(0);

    while (fgets(buf, sizeof(buf), fp) != NULL) {
        l = strlen(buf);
        if (l > 0 && buf[l - 1] == '\n')
            buf[l - 1] = '\0';  // Remove the newline
        l--;

        push_obj(&res, string_from_str(buf, l));
    }

    drop_obj(c);
    status = pclose(fp);

    // Trim res list if it contains only one element
    if (res->len == 0) {
        drop_obj(res);
        res = C8(0);
    }
    if (res->len == 1) {
        c = clone_obj(AS_LIST(res)[0]);
        drop_obj(res);
        res = c;
    }

    // Check command execution status
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        if (res->type == TYPE_LIST) {
            c = clone_obj(AS_LIST(res)[0]);
            drop_obj(res);
            res = c;
        }
        return error_obj(ERR_SYS, res);
    }

    return res;
}