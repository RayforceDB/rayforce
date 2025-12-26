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

#include <limits.h>
#include "io.h"
#include "fs.h"
#include "ops.h"
#include "sock.h"
#include "heap.h"
#include "serde.h"
#include "poll.h"
#include "runtime.h"
#include "error.h"
#include "date.h"
#include "time.h"
#include "timestamp.h"
#include "string.h"
#include "unary.h"
#include "binary.h"
#include "compose.h"
#include "items.h"
#include "ipc.h"
#include "parse.h"

obj_p ray_hopen(obj_p *x, i64_t n) {
    i64_t fd, id, timeout = 0;
    sock_addr_t addr;
    obj_p path, msg, err;

    if (n == 0)
        return err_length(0, 0);

    if (n > 2)
        return err_arity(2, n);

    if (x[0]->type != TYPE_C8)
        return err_type(TYPE_C8, x[0]->type, 0);

    if (n == 2) {
        if (x[1]->type != -TYPE_I64)
            return err_type(-TYPE_I64, x[1]->type, 0);

        timeout = x[1]->i64;
    }

    // Allow only in main thread
    if (!ray_is_main_thread())
        return err_nyi();

    // Open socket
    if (sock_addr_from_str(AS_C8(x[0]), x[0]->len, &addr) != -1) {
        id = ipc_open(runtime_get()->poll, &addr, timeout);

        if (id == -1) {
            msg = cstring_from_str(AS_C8(x[0]), x[0]->len);
            err = err_os();
            drop_obj(msg);
            return err;
        }

        return i64(id);
    }

    // Otherwise, open file
    path = cstring_from_obj(x[0]);
    fd = fs_fopen(AS_C8(path), ATTR_RDWR | ATTR_CREAT | ATTR_APPEND);

    if (fd == -1) {
        err = err_os();
        drop_obj(path);
        return err;
    }

    drop_obj(path);
    return i32((i32_t)fd);
}

obj_p ray_hclose(obj_p x) {
    // Allow only in main thread
    if (!ray_is_main_thread())
        return err_nyi();

    switch (x->type) {
        case -TYPE_I32:
            fs_fclose(x->i32);
            return NULL_OBJ;
        case -TYPE_I64:
            poll_deregister(runtime_get()->poll, x->i64);
            return NULL_OBJ;
        default:
            return err_type(-TYPE_I64, x->type, 0);
    }
}

obj_p ray_read(obj_p x) {
    i64_t sz, rs = 0;
    i64_t fd, size, c = 0;
    u8_t *map, *cur;
    str_p buf;
    obj_p s, v, val, res;

    switch (x->type) {
        case -TYPE_I32:
            fd = x->i32;
            size = fs_fsize(fd);

            if (size < 1)
                return err_length(0, 0);

            // Check for reasonable file size
            if (size > 1000000000)  // 1GB max size
                return err_length(0, 0);

            map = (u8_t *)mmap_file(fd, NULL, size, 0);

            if (map == NULL)
                return err_os();

            // Validate minimum file size for header
            if (size < (i64_t)sizeof(struct ipc_header_t)) {
                mmap_free(map, size);
                return err_os();
            }

            sz = size;

            while (sz > 0) {
                cur = map + sz;
                val = de_raw(cur, &sz);

                if (IS_ERR(val)) {
                    drop_obj(val);
                    break;
                }

                res = eval_obj(val);
                drop_obj(val);

                if (IS_ERR(res)) {
                    mmap_free(map, size);
                    return res;
                }

                drop_obj(res);
                c++;
                rs = size - sz;
            }

            // Cleanup and return results
            mmap_free(map, size);

            v = I64(3);
            AS_I64(v)[0] = c;
            AS_I64(v)[1] = rs;
            AS_I64(v)[2] = size;

            return dict(vn_symbol(3, "items", "read", "total"), v);
        case TYPE_C8:
            s = cstring_from_obj(x);
            fd = fs_fopen(AS_C8(s), ATTR_RDONLY);

            // error handling if file does not exist
            if (fd == -1) {
                res = err_os();
                drop_obj(s);
                return res;
            }

            size = fs_fsize(fd);
            res = C8(size + 1);
            buf = AS_C8(res);
            c = fs_fread(fd, buf, size);
            buf[size] = '\0';
            fs_fclose(fd);

            if (c != size) {
                drop_obj(res);
                res = err_os();
                drop_obj(s);
                return res;
            }

            drop_obj(s);
            return res;
        default:
            return err_type(TYPE_C8, x->type, 0);
    }
}

obj_p io_write(i64_t fd, u8_t msg_type, obj_p obj) {
    obj_p fmt;

    if (obj == NULL_OBJ)
        return NULL_OBJ;

    // don't write to a stdin
    switch (fd) {
        case 0:
            if (obj->type == TYPE_C8)
                return ray_eval_str(obj, NULL);
            else
                return eval_obj(obj);
        case 1:
            fmt = obj_fmt(obj, B8_TRUE);
            fprintf(stdout, "%.*s\n", (i32_t)fmt->len, AS_C8(fmt));
            drop_obj(fmt);
            return NULL_OBJ;
        case 2:
            fmt = obj_fmt(obj, B8_TRUE);
            fprintf(stderr, "%.*s\n", (i32_t)fmt->len, AS_C8(fmt));
            drop_obj(fmt);
            return NULL_OBJ;
        default:
            // send ipc msg

            // Allow only in main thread
            if (!ray_is_main_thread())
                return err_nyi();

            return ipc_send(runtime_get()->poll, fd, obj, msg_type);
    }
}

obj_p ray_write(obj_p x, obj_p y) {
    i64_t size;
    obj_p buf;

    // send to sock handle
    switch (x->type) {
        case -TYPE_I32:
            size = size_obj(y);
            buf = U8(size);
            size = ser_raw(AS_U8(buf), y);
            fs_fwrite(x->i32, AS_C8(buf), size);
            drop_obj(buf);
            return NULL_OBJ;
        case -TYPE_I64:
            // send ipc msg
            if (x->i64 < 0)
                return io_write(-x->i64, MSG_TYPE_ASYN, y);
            else
                return io_write(x->i64, MSG_TYPE_SYNC, y);
        default:
            return err_nyi();
    }
}

obj_p parse_csv_field(i8_t type, str_p start, str_p end, i64_t row, obj_p out) {
    b8_t num_b8 = 0;
    i64_t n, num_i64 = 0;  // Initialize to avoid uninitialized value

    switch (type) {
        case TYPE_B8:
            if (start == NULL || end == NULL) {
                AS_B8(out)[row] = 0;
                break;
            }
            b8_from_str(start, end - start, &num_b8);
            AS_B8(out)[row] = num_b8;
            break;
        case TYPE_U8:
            if (start == NULL || end == NULL) {
                AS_U8(out)[row] = 0;
                break;
            }
            i64_from_str(start, end - start, &num_i64);
            AS_U8(out)[row] = (u8_t)num_i64;
            break;
        case TYPE_I32:
            if (start == NULL || end == NULL) {
                AS_I32(out)[row] = NULL_I32;
                break;
            }
            i32_from_str(start, end - start, &AS_I32(out)[row]);
            break;
        case TYPE_DATE:
            if (start == NULL || end == NULL) {
                AS_DATE(out)[row] = NULL_I32;
                break;
            }
            AS_DATE(out)[row] = date_into_i32(date_from_str(start, end - start));
            break;
        case TYPE_TIME:
            if (start == NULL || end == NULL) {
                AS_TIME(out)[row] = NULL_I32;
                break;
            }
            AS_TIME(out)[row] = time_into_i32(time_from_str(start, end - start));
            break;
        case TYPE_I64:
            if (start == NULL || end == NULL) {
                AS_I64(out)[row] = NULL_I64;
                break;
            }
            i64_from_str(start, end - start, &num_i64);
            AS_I64(out)[row] = num_i64;
            break;
        case TYPE_TIMESTAMP:
            if (start == NULL || end == NULL) {
                AS_TIMESTAMP(out)[row] = NULL_I64;
                break;
            }
            AS_TIMESTAMP(out)[row] = timestamp_into_i64(timestamp_from_str(start, end - start));
            break;
        case TYPE_F64:
            if (start == NULL || end == NULL) {
                AS_F64(out)[row] = NULL_F64;
                break;
            }
            f64_from_str(start, end - start, &AS_F64(out)[row]);
            break;
        case TYPE_SYMBOL:
            if (start == NULL || end == NULL) {
                AS_SYMBOL(out)[row] = 0;
                break;
            }
            n = end - start;
            if ((n > 0) && (*(end - 1) == '\r'))
                n--;

            AS_SYMBOL(out)[row] = symbols_intern(start, n);
            break;
        case TYPE_C8:
            n = end - start;
            if ((n > 0) && (*(end - 1) == '\r'))
                n--;
            AS_LIST(out)[row] = string_from_str(start, n);
            break;
        case TYPE_GUID:
            if (start == NULL || end == NULL) {
                memcpy(AS_GUID(out)[row], NULL_GUID, sizeof(guid_t));
                break;
            }
            if (guid_from_str(start, end - start, AS_GUID(out)[row]) == -1) {
                memcpy(AS_GUID(out)[row], NULL_GUID, sizeof(guid_t));
                break;
            }
            break;
        default:
            return err_type(TYPE_LIST, type, 0);
    }

    return NULL_OBJ;
}

obj_p parse_csv_line(i8_t types[], i64_t cnt, str_p start, str_p end, i64_t row, obj_p cols, c8_t sep) {
    i64_t i, len;
    str_p prev, pos;
    obj_p res;

    for (i = 0, pos = start; i < cnt; i++) {
        if (pos == NULL || end == NULL) {
            res = parse_csv_field(types[i], NULL, NULL, row, AS_LIST(cols)[i]);

            if (!is_null(res))
                return res;

            continue;
        }

        len = end - pos;
        prev = pos;

        // quoted field?
        if (len > 0 && *prev == '"') {
            prev++;
            pos = (str_p)memchr(prev, '"', len - 1);

            if (pos == NULL)
                return err_length(0, 0);

            res = parse_csv_field(types[i], prev, pos, row, AS_LIST(cols)[i]);
            pos += 2;  // skip quote and comma

            if (!is_null(res))
                return res;

            continue;
        }

        if (len == 0) {
            res = parse_csv_field(types[i], NULL, NULL, row, AS_LIST(cols)[i]);

            if (!is_null(res))
                return res;

            continue;
        }

        pos = (str_p)memchr(pos, sep, len);
        if (pos == NULL) {
            if (i < cnt - 1)
                return err_length(0, 0);
            pos = end;
        }

        res = parse_csv_field(types[i], prev, pos, row, AS_LIST(cols)[i]);

        if (!is_null(res))
            return res;

        pos++;
    }

    return NULL_OBJ;
}

obj_p parse_csv_range(i8_t types[], i64_t num_types, str_p buf, i64_t size, i64_t lines, i64_t start_line, obj_p cols,
                      c8_t sep) {
    i64_t i, j, k, l;
    str_p line_end, prev;
    obj_p res = NULL_OBJ;

    for (i = 0, prev = buf; i < lines; i++) {
        line_end = (str_p)memchr(prev, '\n', buf + size - prev);
        if (line_end == NULL) {
            line_end = prev + size;  // Handle last line without newline
            if (line_end > buf && *(line_end - 1) == '\r')
                line_end--;  // Handle carriage return
        }

        res = parse_csv_line(types, num_types, prev, line_end, start_line + i, cols, sep);

        if (res != NULL_OBJ) {
            l = cols->len;
            for (j = 0; j < l; j++) {
                // Need to free allocated objects in case of compound type
                if (AS_LIST(cols)[j]->type == TYPE_LIST) {
                    for (k = 0; k < i; k++) {
                        drop_obj(AS_LIST(AS_LIST(cols)[j])[k]);
                    }
                }
                return res;
            }
        }

        prev = line_end + 1;  // Move past the newline
    }

    return NULL_OBJ;  // Success
}

obj_p parse_csv_lines(i8_t *types, i64_t num_types, str_p buf, i64_t size, i64_t total_lines, obj_p cols, c8_t sep) {
    obj_p err, res = NULL_OBJ;
    i64_t i, l, batch_size, num_batches, lines_per_batch, start_line, end_line, lines_in_batch;
    str_p batch_start, batch_end;
    pool_p pool = runtime_get()->pool;

    num_batches = pool_split_by(pool, total_lines, 0);

    if (num_batches == 1)
        return parse_csv_range(types, num_types, buf, size, total_lines, 0, cols, sep);

    pool_prepare(pool);

    // --- PAGE-ALIGNED CHUNKING ---
    i64_t page_size = RAY_PAGE_SIZE;
    // Estimate average line length (avoid div by zero)
    i64_t avg_line_len = size / (total_lines ? total_lines : 1);
    i64_t lines_per_page = (avg_line_len > 0) ? (page_size / avg_line_len) : 1;
    if (lines_per_page == 0)
        lines_per_page = 1;
    lines_per_batch = (total_lines + num_batches - 1) / num_batches;
    // round up to nearest multiple of lines_per_page
    lines_per_batch = ((lines_per_batch + lines_per_page - 1) / lines_per_page) * lines_per_page;

    for (start_line = 0; start_line < total_lines; start_line = end_line) {
        end_line = start_line + lines_per_batch;
        if (end_line > total_lines)
            end_line = total_lines;
        lines_in_batch = end_line - start_line;

        // Now calculate the actual buffer positions for start_line and end_line
        batch_start = buf;
        for (i = 0; i < start_line && batch_start < (buf + size); ++i)
            batch_start = (str_p)memchr(batch_start, '\n', size - (batch_start - buf)) + 1;

        batch_end = batch_start;
        for (i = 0; i < lines_in_batch && batch_end < (buf + size); ++i) {
            batch_end = (str_p)memchr(batch_end, '\n', size - (batch_end - buf));
            if (batch_end == NULL) {
                batch_end = buf + size;  // Handle the last line in the buffer
                break;
            } else {
                ++batch_end;  // Move past the newline to include the full line
            }
        }

        // Handle cases where lines are fewer than expected or parsing issues
        if (batch_start == NULL || batch_end == NULL || batch_start >= batch_end)
            break;

        batch_size = batch_end - batch_start;
        pool_add_task(pool, (raw_p)parse_csv_range, 8, types, num_types, batch_start, batch_size, lines_in_batch,
                      start_line, cols, sep);
    }

    res = pool_run(pool);

    l = res->len;
    for (i = 0; i < l; i++) {
        if (IS_ERR(AS_LIST(res)[i])) {
            err = clone_obj(AS_LIST(res)[i]);
            drop_obj(res);
            return err;
        }
    }

    drop_obj(res);

    return NULL_OBJ;  // Success
}

static i64_t symbol_from_str_trimmed(str_p src, i64_t len) {
    c8_t buf[256];  // Stack buffer for common case
    c8_t *trimmed;
    i64_t i, j = 0;
    i64_t result;
    c8_t c;

    // Fast path: use stack buffer for small strings
    if (len <= 256) {
        trimmed = buf;
    } else {
        // Fallback to heap for large strings
        trimmed = (c8_t *)heap_alloc(len);
        if (trimmed == NULL)
            return NULL_I64;
    }

    // Filter: keep only alphabetical characters (A-Z, a-z)
    for (i = 0; i < len; i++) {
        c = src[i];
        // Manual check for alphabetic characters for better performance
        // This is faster than isalpha() and avoids locale-dependent behavior
        if (is_alphanum(c))
            trimmed[j++] = c;
    }

    // Intern the trimmed string
    result = symbols_intern(trimmed, j);

    // Free heap-allocated buffer if used
    if (trimmed != buf)
        heap_free(trimmed);

    return result;
}

obj_p ray_read_csv(obj_p *x, i64_t n) {
    i64_t fd, size;
    i64_t i, l, len, lines;
    str_p buf, prev, pos, line;
    obj_p types, names, cols, path, res;
    i8_t type;
    c8_t sep = ',';

    switch (n) {
        case 2:
        case 3:
            if (n == 3) {
                if (x[2]->type != -TYPE_C8)
                    return err_type(-TYPE_C8, x[2]->type, 0);

                sep = x[2]->u8;
            }
            // expect vector of types as 1st arg:
            if (x[0]->type != TYPE_SYMBOL)
                return err_type(TYPE_SYMBOL, x[0]->type, 0);

            // expect string as 2nd arg:
            if (x[1]->type != TYPE_C8)
                return err_type(TYPE_C8, x[1]->type, 0);

            // check that all symbols are valid typenames and convert them to types
            l = x[0]->len;
            types = U8(l);
            for (i = 0; i < l; i++) {
                type = env_get_type_by_type_name(&runtime_get()->env, AS_SYMBOL(x[0])[i]);
                if (type == TYPE_ERR) {
                    drop_obj(types);
                    return err_value(AS_SYMBOL(x[0])[i]);
                }

                if (type < 0)
                    type = -type;

                types->raw[i] = type;
            }

            path = cstring_from_obj(x[1]);
            fd = fs_fopen(AS_C8(path), ATTR_RDWR);

            if (fd == -1) {
                drop_obj(types);
                res = err_os();
                drop_obj(path);
                return res;
            }

            size = fs_fsize(fd);

            if (size == -1) {
                drop_obj(types);
                fs_fclose(fd);
                drop_obj(path);
                return err_length(0, 0);
            }

            buf = (str_p)mmap_file(fd, NULL, size, 0);

            if (buf == NULL) {
                drop_obj(types);
                fs_fclose(fd);
                return err_os();
            }

            pos = buf;
            lines = 0;

            while ((pos = (str_p)memchr(pos, '\n', buf + size - pos))) {
                ++lines;
                ++pos;  // Move past the newline character
            }

            if (lines == 0) {
                drop_obj(types);
                fs_fclose(fd);
                mmap_free(buf, size);
                drop_obj(path);
                return err_length(0, 0);
            }

            // Adjust for the file not ending with a newline
            if (size > 0 && buf[size - 1] != '\n')
                ++lines;

            // parse header
            pos = (str_p)memchr(buf, '\n', size);
            len = pos - buf;
            line = pos + 1;

            names = SYMBOL(l);
            pos = buf;

            for (i = 0; i < l; i++) {
                prev = pos;
                pos = (str_p)memchr(pos, sep, len);
                if (pos == NULL) {
                    if (i < l - 1) {
                        drop_obj(types);
                        drop_obj(names);
                        fs_fclose(fd);
                        mmap_free(buf, size);
                        return err_length(0, 0);
                    }

                    pos = prev + len;
                    // truncate \r (if any)
                    if ((len > 0) && (pos[-1] == '\r'))
                        pos--;
                }

                AS_SYMBOL(names)[i] = symbol_from_str_trimmed(prev, pos - prev);
                pos++;
                len -= (pos - prev);
            }

            if (i != l) {
                drop_obj(types);
                drop_obj(names);
                fs_fclose(fd);
                mmap_free(buf, size);
                drop_obj(path);
                return err_length(0, 0);
            }

            // exclude header
            lines--;

            // allocate columns
            cols = LIST(l);
            for (i = 0; i < l; i++) {
                if (AS_C8(types)[i] == TYPE_C8)
                    AS_LIST(cols)[i] = LIST(lines);
                else
                    AS_LIST(cols)[i] = vector(AS_C8(types)[i], lines);
            }

            // parse lines
            res = parse_csv_lines((i8_t *)AS_U8(types), l, line, size, lines, cols, sep);

            drop_obj(types);
            fs_fclose(fd);
            mmap_free(buf, size);
            drop_obj(path);

            if (!is_null(res)) {
                drop_obj(names);
                l = cols->len;
                for (i = 0; i < l; i++)
                    AS_LIST(cols)[i]->len = 0;

                drop_obj(cols);
                return res;
            }

            return table(names, cols);
        default:
            return err_length(0, 0);
    }
}

static i64_t csv_field_to_str(obj_p col, i64_t row, obj_p *dst) {
    i64_t n;
    str_p s;
    obj_p str_obj;

    switch (col->type) {
        case TYPE_B8:
            return AS_B8(col)[row] ? str_fmt_into(dst, -1, "true") : str_fmt_into(dst, -1, "false");
        case TYPE_U8:
            return str_fmt_into(dst, -1, "%u", AS_U8(col)[row]);
        case TYPE_I32:
            if (AS_I32(col)[row] == NULL_I32)
                return 0;
            return str_fmt_into(dst, -1, "%d", AS_I32(col)[row]);
        case TYPE_DATE:
            if (AS_DATE(col)[row] == NULL_I32)
                return 0;
            {
                datestruct_t dt = date_from_i32(AS_DATE(col)[row]);
                return str_fmt_into(dst, -1, "%.4d.%.2d.%.2d", dt.year, dt.month, dt.day);
            }
        case TYPE_TIME:
            if (AS_TIME(col)[row] == NULL_I32)
                return 0;
            {
                timestruct_t tm = time_from_i32(AS_TIME(col)[row]);
                if (tm.sign == -1)
                    return str_fmt_into(dst, -1, "-%.2d:%.2d:%.2d.%.3d", tm.hours, tm.mins, tm.secs, tm.msecs);
                return str_fmt_into(dst, -1, "%.2d:%.2d:%.2d.%.3d", tm.hours, tm.mins, tm.secs, tm.msecs);
            }
        case TYPE_I64:
            if (AS_I64(col)[row] == NULL_I64)
                return 0;
            return str_fmt_into(dst, -1, "%lld", AS_I64(col)[row]);
        case TYPE_TIMESTAMP:
            if (AS_TIMESTAMP(col)[row] == NULL_I64)
                return 0;
            {
                timestamp_t ts = timestamp_from_i64(AS_TIMESTAMP(col)[row]);
                return str_fmt_into(dst, -1, "%.4d.%.2d.%.2dD%.2d:%.2d:%.2d.%.9d", ts.year, ts.month, ts.day, ts.hours,
                                    ts.mins, ts.secs, ts.nanos);
            }
        case TYPE_F64:
            if (ISNANF64(AS_F64(col)[row]))
                return 0;
            return str_fmt_into(dst, -1, "%g", AS_F64(col)[row]);
        case TYPE_SYMBOL:
            if (AS_SYMBOL(col)[row] == NULL_I64)
                return 0;
            s = str_from_symbol(AS_SYMBOL(col)[row]);
            return str_fmt_into(dst, -1, "%s", s);
        case TYPE_C8:
            // String type - output with quotes if it contains separator or quotes
            s = AS_C8(col);
            n = col->len;
            return str_fmt_into(dst, -1, "%.*s", (i32_t)n, s);
        case TYPE_LIST:
            // List of strings
            str_obj = AS_LIST(col)[row];
            if (str_obj == NULL_OBJ || str_obj->type == TYPE_NULL)
                return 0;
            s = AS_C8(str_obj);
            n = str_obj->len;
            // Check if we need to quote (contains separator, newline, or quote)
            {
                i64_t i;
                b8_t need_quote = B8_FALSE;
                for (i = 0; i < n; i++) {
                    if (s[i] == ',' || s[i] == '"' || s[i] == '\n' || s[i] == '\r') {
                        need_quote = B8_TRUE;
                        break;
                    }
                }
                if (need_quote) {
                    i64_t m = str_fmt_into(dst, -1, "\"");
                    for (i = 0; i < n; i++) {
                        if (s[i] == '"')
                            m += str_fmt_into(dst, -1, "\"\"");
                        else
                            m += str_fmt_into(dst, -1, "%c", s[i]);
                    }
                    m += str_fmt_into(dst, -1, "\"");
                    return m;
                }
            }
            return str_fmt_into(dst, -1, "%.*s", (i32_t)n, s);
        case TYPE_GUID:
            if (memcmp(AS_GUID(col)[row], NULL_GUID, sizeof(guid_t)) == 0)
                return 0;
            {
                guid_t *g = &AS_GUID(col)[row];
                return str_fmt_into(dst, -1, "%02hhx%02hhx%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                                    (*g)[0], (*g)[1], (*g)[2], (*g)[3], (*g)[4], (*g)[5], (*g)[6], (*g)[7], (*g)[8],
                                    (*g)[9], (*g)[10], (*g)[11], (*g)[12], (*g)[13], (*g)[14], (*g)[15]);
            }
        default:
            return 0;
    }
}

obj_p ray_write_csv(obj_p *x, i64_t n) {
    i64_t fd, c;
    i64_t i, j, l, rows;
    obj_p table, path, names, cols, col, buf, res;
    c8_t sep = ',';

    switch (n) {
        case 2:
        case 3:
            if (n == 3) {
                if (x[2]->type != -TYPE_C8)
                    return err_type(-TYPE_C8, x[2]->type, 0);

                sep = x[2]->c8;
            }

            // expect string as 1st arg:
            if (x[0]->type != TYPE_C8)
                return err_type(TYPE_C8, x[0]->type, 0);

            // expect table as 2nd arg:
            if (x[1]->type != TYPE_TABLE)
                return err_type(TYPE_TABLE, x[1]->type, 0);

            table = x[1];
            names = AS_LIST(table)[0];
            cols = AS_LIST(table)[1];
            l = names->len;

            if (l == 0)
                return err_length(0, 0);

            rows = ops_count(table);

            // Open file for writing
            path = cstring_from_obj(x[0]);
            fd = fs_fopen(AS_C8(path), ATTR_WRONLY | ATTR_CREAT | ATTR_TRUNC);

            if (fd == -1) {
                res = err_os();
                drop_obj(path);
                return res;
            }

            buf = NULL_OBJ;

            // Write header row
            for (i = 0; i < l; i++) {
                str_fmt_into(&buf, -1, "%s", str_from_symbol(AS_SYMBOL(names)[i]));
                if (i < l - 1)
                    str_fmt_into(&buf, -1, "%c", sep);
            }
            str_fmt_into(&buf, -1, "\n");

            // Write data rows
            for (j = 0; j < rows; j++) {
                for (i = 0; i < l; i++) {
                    col = AS_LIST(cols)[i];
                    csv_field_to_str(col, j, &buf);
                    if (i < l - 1)
                        str_fmt_into(&buf, -1, "%c", sep);
                }
                str_fmt_into(&buf, -1, "\n");
            }

            // Write buffer to file
            c = fs_fwrite(fd, AS_C8(buf), buf->len);
            if (c == -1) {
                res = err_os();
                drop_obj(buf);
                drop_obj(path);
                fs_fclose(fd);
                return res;
            }

            drop_obj(buf);
            drop_obj(path);
            fs_fclose(fd);

            return NULL_OBJ;
        default:
            return err_length(0, 0);
    }
}

obj_p ray_parse(obj_p x) {
    obj_p s, res;

    if (!x || x->type != TYPE_C8)
        return err_type(TYPE_C8, x ? x->type : 0, 0);

    s = cstring_from_obj(x);

    res = ray_parse_str(0, s, NULL_OBJ);
    drop_obj(s);

    return res;
}

obj_p ray_eval(obj_p x) {
    switch (x->type) {
        case TYPE_C8:
            return ray_eval_str(x, NULL_OBJ);
        default:
            return eval_obj(x);
    }
}

obj_p ray_load(obj_p x) {
    i64_t flen;
    obj_p file, sym, tab, res;
    lit_p fname;

    if (!x || x->type != TYPE_C8)
        return err_type(TYPE_C8, x ? x->type : 0, 0);

    // table expected
    if (x->len > 1 && AS_C8(x)[x->len - 1] == '/') {
        tab = ray_get(x);
        if (IS_ERR(tab))
            return tab;

        // bind table to a symbol from the path, so determine the last part of the path
        file = cstring_from_obj(x);
        flen = fs_filename(AS_C8(file), &fname);
        sym = symbol(fname, flen);
        res = ray_set(sym, tab);

        drop_obj(file);
        drop_obj(sym);
        drop_obj(tab);

        return res;
    }

    file = ray_read(x);
    if (IS_ERR(file))
        return file;

    res = ray_eval_str(file, x);
    drop_obj(file);

    return res;
}

obj_p distinct_syms(obj_p *x, i64_t n) {
    i64_t p;
    i64_t i, j, h, l;
    obj_p vec, set, a;

    if (n == 0 || (*x)->len == 0)
        return SYMBOL(0);

    l = (*x)->len;

    set = ht_oa_create(l, -1);

    for (i = 0, h = 0; i < n; i++) {
        a = *(x + i);
        for (j = 0; j < l; j++) {
            p = ht_oa_tab_next(&set, AS_SYMBOL(a)[j]);
            if (AS_SYMBOL(AS_LIST(set)[0])[p] == NULL_I64) {
                AS_SYMBOL(AS_LIST(set)[0])[p] = AS_SYMBOL(a)[j];
                h++;
            }
        }
    }

    vec = SYMBOL(h);
    l = AS_LIST(set)[0]->len;

    for (i = 0, j = 0; i < l; i++) {
        if (AS_SYMBOL(AS_LIST(set)[0])[i] != NULL_I64)
            AS_SYMBOL(vec)[j++] = AS_SYMBOL(AS_LIST(set)[0])[i];
    }

    vec->attrs |= ATTR_DISTINCT;

    drop_obj(set);

    return vec;
}

obj_p io_get_symfile(obj_p path) {
    obj_p s, col, v;

    if (path->len < 2 || AS_C8(path)[path->len - 1] != '/') {
        v = ray_get(path);
    } else {
        s = string_from_str("sym", 3);
        col = ray_concat(path, s);
        v = ray_get(col);
        drop_obj(s);
        drop_obj(col);
    }

    if (IS_ERR(v))
        return v;

    s = symbol("sym", 3);
    drop_obj(ray_set(s, v));
    drop_obj(s);
    drop_obj(v);

    return NULL_OBJ;
}

obj_p io_set_table(obj_p path, obj_p table) {
    i64_t c, fd;
    obj_p s, buf, res;

    // Save splayed
    if (path->len > 0 && AS_C8(path)[path->len - 1] == '/')
        return io_set_table_splayed(path, table, NULL_OBJ);

    // Save as a blob (serialized)
    s = cstring_from_obj(path);
    fd = fs_fopen(AS_C8(s), ATTR_WRONLY | ATTR_CREAT | ATTR_TRUNC);

    if (fd == -1) {
        res = err_os();
        drop_obj(s);
        return res;
    }

    buf = ser_obj(table);
    if (IS_ERR(buf)) {
        drop_obj(s);
        fs_fclose(fd);
        return buf;
    }

    c = fs_fwrite(fd, AS_C8(buf), buf->len);
    if (c == -1) {
        res = err_os();
        drop_obj(buf);
        drop_obj(s);
        fs_fclose(fd);
        return res;
    }

    drop_obj(s);
    drop_obj(buf);
    fs_fclose(fd);

    return NULL_OBJ;
}

obj_p io_set_table_splayed(obj_p path, obj_p table, obj_p symfile) {
    i64_t i, l;
    obj_p res, col, s, p, v, e, cols, sym;

    // save columns schema
    s = cstring_from_str(".d", 2);
    col = ray_concat(path, s);
    res = binary_set(col, AS_LIST(table)[0]);

    drop_obj(s);
    drop_obj(col);

    if (IS_ERR(res))
        return res;

    drop_obj(res);

    l = AS_LIST(table)[0]->len;

    cols = LIST(0);

    // find symbol columns
    for (i = 0; i < l; i++) {
        if (AS_LIST(AS_LIST(table)[1])[i]->type == TYPE_SYMBOL)
            push_obj(&cols, clone_obj(AS_LIST(AS_LIST(table)[1])[i]));
    }

    sym = distinct_syms(AS_LIST(cols), cols->len);

    if (sym->len > 0) {
        switch (symfile->type) {
            case TYPE_NULL:
                s = cstring_from_str("sym", 3);
                col = ray_concat(path, s);
                res = binary_set(col, sym);
                drop_obj(s);
                drop_obj(col);
                break;
            case TYPE_C8:
                // First we need to ckeck if symfile is already exist, if so - we need to merge
                // it with sym and enumerate symbol columns over the whole new sym file, preserving ids of the old
                // ones
                s = ray_get(symfile);
                if (s->type == TYPE_SYMBOL) {
                    // First retrieve distinct symbols from the new columns not present in current sym file
                    v = ray_except(sym, s);
                    drop_obj(sym);
                    sym = ray_concat(s, v);
                    drop_obj(v);
                }

                drop_obj(s);
                res = binary_set(symfile, sym);
                break;
            default:
                drop_obj(cols);
                drop_obj(sym);
                return err_type(TYPE_C8, symfile->type, 0);
        }

        if (IS_ERR(res))
            return res;

        drop_obj(res);

        s = symbol("sym", 3);
        res = binary_set(s, sym);

        drop_obj(s);

        if (IS_ERR(res))
            return res;

        drop_obj(res);
    }

    drop_obj(cols);
    drop_obj(sym);
    // --

    // save columns data
    for (i = 0; i < l; i++) {
        v = at_idx(AS_LIST(table)[1], i);

        // symbol column need to be converted to enum
        if (v->type == TYPE_SYMBOL) {
            s = symbol("sym", 3);
            e = ray_enum(s, v);
            drop_obj(s);
            drop_obj(v);

            if (IS_ERR(e))
                return e;

            v = e;
        }

        p = at_idx(AS_LIST(table)[0], i);
        s = cast_obj(TYPE_C8, p);
        col = ray_concat(path, s);
        res = binary_set(col, v);

        drop_obj(p);
        drop_obj(v);
        drop_obj(s);
        drop_obj(col);

        if (IS_ERR(res))
            return res;

        drop_obj(res);
    }

    return clone_obj(path);
}

obj_p io_get_table_splayed(obj_p path, obj_p symfile) {
    obj_p col, keys, vals, val, s, v;
    i64_t i, l;
    b8_t syms_present = B8_FALSE;

    // first try to read columns schema
    s = cstring_from_str(".d", 2);
    col = ray_concat(path, s);
    keys = ray_get(col);
    drop_obj(s);
    drop_obj(col);

    if (IS_ERR(keys))
        return keys;

    if (keys->type != TYPE_SYMBOL) {
        i8_t actual = keys->type;
        drop_obj(keys);
        return err_type(TYPE_SYMBOL, actual, 0);
    }

    l = keys->len;
    vals = LIST(l);

    for (i = 0; i < l; i++) {
        v = at_idx(keys, i);
        s = cast_obj(TYPE_C8, v);
        col = ray_concat(path, s);
        val = ray_get(col);

        drop_obj(v);
        drop_obj(s);
        drop_obj(col);

        if (IS_ERR(val)) {
            vals->len = i;
            drop_obj(vals);
            drop_obj(keys);

            return val;
        }

        AS_LIST(vals)[i] = val;

        if (val->type == TYPE_ENUM)
            syms_present = B8_TRUE;
    }

    // read symbol data (if any) if sym is not present in current env
    // symfile is optional - ignore error if it doesn't exist
    if (syms_present && resolve(SYMBOL_SYM) == NULL)
        drop_obj((symfile->type != TYPE_NULL) ? io_get_symfile(symfile) : io_get_symfile(path));

    return table(keys, vals);
}
