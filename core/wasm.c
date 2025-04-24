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

#include <emscripten.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include "parse.h"
#include "hash.h"
#include "format.h"
#include "util.h"
#include "string.h"
#include "poll.h"
#include "heap.h"
#include "runtime.h"
#include "sys.h"
#include "fs.h"

#define __ABOUT \
    "\
  %s%sRayforceDB: %d.%d %s\n\
  WASM target\n\
  Started from: %s\n\
  Documentation: https://rayforcedb.com/\n\
  Github: https://github.com/singaraiona/rayforce%s\n"

// Declare rayforce_ready callback on js side
EM_JS(nil_t, js_rayforce_ready, (str_p text), { Module.rayforce_ready(UTF8ToC8(text)); });

poll_p poll_init(i64_t port) {
    poll_p poll = (poll_p)heap_alloc(sizeof(struct poll_t));
    poll->code = NULL_I64;

    return poll;
}

nil_t poll_destroy(poll_p poll) { heap_free(poll); }

i64_t poll_run(poll_p poll) {
    UNUSED(poll);
    return 0;
}

i64_t poll_register(poll_p poll, i64_t fd) {
    UNUSED(poll);
    UNUSED(fd);

    return -1;
}

i64_t poll_register_with_callbacks(poll_p poll, i64_t fd, on_read_callback_t on_read, on_write_callback_t on_write,
                                   on_error_callback_t on_error, raw_p user_data) {
    UNUSED(poll);
    UNUSED(fd);
    UNUSED(on_read);
    UNUSED(on_write);
    UNUSED(on_error);
    UNUSED(user_data);

    return -1;
}

nil_t poll_deregister(poll_p poll, i64_t id) {
    UNUSED(poll);
    UNUSED(id);
}

obj_p ipc_send_sync(poll_p poll, i64_t id, obj_p msg) {
    UNUSED(poll);
    UNUSED(id);
    UNUSED(msg);
    return NULL_OBJ;
}

obj_p ipc_send_async(poll_p poll, i64_t id, obj_p msg) {
    UNUSED(poll);
    UNUSED(id);
    UNUSED(msg);
    return NULL_OBJ;
}

nil_t list_examples(obj_p *dst) {
    DIR *dir;
    struct dirent *entry;

    str_fmt_into(dst, -1, "\n  -- Here is the list of examples:\n");

    // Attempt to open the directory
    dir = opendir("examples/");
    if (dir == NULL)
        return;

    // Read each entry in the directory
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, ".", 1) == 0 || strncmp(entry->d_name, "..", 2) == 0)
            continue;
        str_fmt_into(dst, -1, "  |- %s\n", entry->d_name);
    }

    // Close the directory
    closedir(dir);

    str_fmt_into(dst, -1, "  -- To try an example, type: (load \"examples/<example_name>)\"\n");

    return;
}

EMSCRIPTEN_KEEPALIVE str_p strof_obj(obj_p obj) { return AS_C8(obj); }

EMSCRIPTEN_KEEPALIVE i32_t main(i32_t argc, str_p argv[]) {
    i32_t code;
    sys_info_t info = sys_info(1);
    obj_p fmt = NULL_OBJ;

    str_fmt_into(&fmt, -1, __ABOUT, BOLD, YELLOW, info.major_version, info.minor_version, info.build_date, info.cwd,
                 RESET);

    list_examples(&fmt);

    atexit(runtime_destroy);
    runtime_create(0, NULL);
    code = runtime_run();
    js_rayforce_ready(AS_C8(fmt));
    drop_obj(fmt);

    return code;
}
