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

#include <errno.h>
#include <limits.h>
#include "io.h"
#include "fs.h"
#include "util.h"
#include "ops.h"
#include "sock.h"
#include "heap.h"
#include "serde.h"
#include "poll.h"
#include "runtime.h"
#include "error.h"
#include "timestamp.h"
#include "sys.h"
#include "string.h"
#include "unary.h"
#include "binary.h"
#include "compose.h"
#include "items.h"

obj_p ray_hopen(obj_p x) {
    i64_t fd;
    sock_addr_t addr;
    obj_p err;
    u8_t handshake[2] = {RAYFORCE_VERSION, 0x00};

    if (x->type != TYPE_C8)
        THROW(ERR_TYPE, "hopen: expected char");

    if (sock_addr_from_str(AS_C8(x), &addr) == -1)
        THROW(ERR_IO, "hopen: invalid address: %s", AS_C8(x));

    fd = sock_open(&addr);

    if (fd == -1)
        return sys_error(ERROR_TYPE_SOCK, "hopen");

    if (sock_send(fd, handshake, 2) == -1) {
        err = sys_error(ERROR_TYPE_SOCK, "hopen: send handshake");
        sock_close(fd);
        return err;
    }

    if (sock_recv(fd, handshake, 2) == -1) {
        err = sys_error(ERROR_TYPE_SOCK, "hopen: recv handshake");
        sock_close(fd);
        return err;
    }

    sock_set_nonblocking(fd, B8_TRUE);

    return i64(poll_register(runtime_get()->poll, fd, RAYFORCE_VERSION));
}

obj_p ray_hclose(obj_p x) {
    poll_deregister(runtime_get()->poll, x->i64);

    return NULL_OBJ;
}

obj_p ray_read(obj_p x) {
    i64_t fd, size, c = 0;
    str_p buf;
    obj_p s, res;

    switch (x->type) {
        case TYPE_C8:
            s = cstring_from_obj(x);
            fd = fs_fopen(AS_C8(s), ATTR_RDONLY);

            // error handling if file does not exist
            if (fd == -1) {
                res = sys_error(ERROR_TYPE_SYS, AS_C8(s));
                drop_obj(s);
                return res;
            }

            size = fs_fsize(fd);
            res = C8(size + 1);
            buf = AS_C8(res);
            c = fs_fread(fd, buf, size);
            fs_fclose(fd);

            if (c != size) {
                drop_obj(res);
                res = sys_error(ERROR_TYPE_SYS, AS_C8(s));
                drop_obj(s);
                return res;
            }

            drop_obj(s);

            return res;
        default:
            THROW(ERR_TYPE, "read: unsupported type: '%s", type_name(x->type));
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
            switch (msg_type) {
                case MSG_TYPE_RESP:
                    return ipc_send_async(runtime_get()->poll, fd, clone_obj(obj));
                case MSG_TYPE_ASYN:
                    return ipc_send_async(runtime_get()->poll, fd, clone_obj(obj));
                case MSG_TYPE_SYNC:
                    return ipc_send_sync(runtime_get()->poll, fd, clone_obj(obj));
                default:
                    THROW(ERR_TYPE, "write: unsupported msg type: '%d", msg_type);
            }
    }
}

obj_p ray_write(obj_p x, obj_p y) {
    // send to sock handle
    if (x->type == -TYPE_I64) {
        // send ipc msg
        if (x->i64 < 0)
            return io_write(-x->i64, MSG_TYPE_ASYN, y);
        else
            return io_write(x->i64, MSG_TYPE_SYNC, y);
    }

    THROW(ERR_NOT_IMPLEMENTED, "write: not implemented");
}

obj_p parse_csv_field(i8_t type, str_p start, str_p end, i64_t row, obj_p out) {
    i64_t n;

    switch (type) {
        case TYPE_U8:
            if (start == NULL || end == NULL) {
                AS_U8(out)[row] = 0;
                break;
            }
            AS_U8(out)[row] = (u8_t)i64_from_str(start, end - start);
            break;
        case TYPE_I64:
            AS_I64(out)[row] = i64_from_str(start, end - start);
            break;
        case TYPE_TIMESTAMP:
            if (start == NULL || end == NULL) {
                AS_TIMESTAMP(out)[row] = NULL_I64;
                break;
            }
            AS_TIMESTAMP(out)[row] = timestamp_into_i64(timestamp_from_str(start, end - start));
            break;
        case TYPE_F64:
            AS_F64(out)[row] = f64_from_str(start, end - start);
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
            THROW(ERR_TYPE, "csv: unsupported type: '%s", type_name(type));
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
                THROW(ERR_LENGTH, "csv: line: %lld invalid input: %s", row + 1, prev);

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
                THROW(ERR_LENGTH, "csv: line: %lld invalid input: %s", row + 1, prev);
            pos = end;
        }

        res = parse_csv_field(types[i], prev, pos, row, AS_LIST(cols)[i]);

        if (!is_null(res))
            return res;

        pos++;
    }

    return NULL_OBJ;
}

obj_p parse_csv_range(i8_t *types, i64_t num_types, str_p buf, i64_t size, i64_t lines, i64_t start_line, obj_p cols,
                      c8_t sep) {
    str_p line_end, prev;
    obj_p res = NULL_OBJ;
    i64_t j;

    for (j = 0, prev = buf; j < lines; j++) {
        line_end = (str_p)memchr(prev, '\n', buf + size - prev);
        if (line_end == NULL) {
            line_end = prev + size;  // Handle last line without newline
            if (line_end > buf && *(line_end - 1) == '\r')
                line_end--;  // Handle carriage return
        }

        res = parse_csv_line(types, num_types, prev, line_end, start_line + j, cols, sep);
        if (res != NULL_OBJ)
            return res;

        prev = line_end + 1;  // Move past the newline
    }

    return NULL_OBJ;  // Success
}

obj_p parse_csv_lines(i8_t *types, i64_t num_types, str_p buf, i64_t size, i64_t total_lines, obj_p cols, c8_t sep) {
    obj_p res = NULL_OBJ;
    i64_t i, batch, batch_size, num_batches, lines_per_batch, start_line, end_line, lines_in_batch;
    str_p batch_start, batch_end;
    pool_p pool = runtime_get()->pool;

    num_batches = pool_split_by(pool, total_lines, 0);

    if (num_batches == 1)
        return parse_csv_range(types, num_types, buf, size, total_lines, 0, cols, sep);

    pool_prepare(pool);

    lines_per_batch = (total_lines + num_batches - 1) / num_batches;  // Calculate lines per batch

    for (batch = 0; batch < num_batches; ++batch) {
        start_line = batch * lines_per_batch;
        end_line = start_line + lines_per_batch;
        if (batch == num_batches - 1 || end_line > total_lines)
            end_line = total_lines;  // Adjust the last batch to cover all remaining lines

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

    if (IS_ERROR(res))
        return res;

    drop_obj(res);

    return NULL_OBJ;  // Success
}

obj_p ray_read_csv(obj_p *x, i64_t n) {
    i64_t i, l, fd, len, lines, size;
    str_p buf, prev, pos, line;
    obj_p types, names, cols, path, res;
    i8_t type;
    c8_t sep = ',';

    switch (n) {
        case 2:
        case 3:
            if (n == 3) {
                if (x[2]->type != -TYPE_C8)
                    THROW(ERR_TYPE, "csv: expected 'char' as 3rd argument, got: '%s", type_name(x[2]->type));

                sep = x[2]->u8;
            }
            // expect vector of types as 1st arg:
            if (x[0]->type != TYPE_SYMBOL)
                THROW(ERR_TYPE, "csv: expected vector of types as 1st argument, got: '%s", type_name(x[0]->type));

            // expect string as 2nd arg:
            if (x[1]->type != TYPE_C8)
                THROW(ERR_TYPE, "csv: expected string as 2nd argument, got: '%s", type_name(x[1]->type));

            // check that all symbols are valid typenames and convert them to types
            l = x[0]->len;
            types = U8(l);
            for (i = 0; i < l; i++) {
                type = env_get_type_by_type_name(&runtime_get()->env, AS_SYMBOL(x[0])[i]);
                if (type == TYPE_ERROR) {
                    drop_obj(types);
                    THROW(ERR_TYPE, "csv: invalid type: '%s", str_from_symbol(AS_SYMBOL(x[0])[i]));
                }

                if (type < 0)
                    type = -type;

                types->arr[i] = type;
            }

            path = cstring_from_obj(x[1]);
            fd = fs_fopen(AS_C8(path), ATTR_RDONLY);

            if (fd == -1) {
                drop_obj(types);
                res = sys_error(ERROR_TYPE_SYS, AS_C8(path));
                drop_obj(path);
                return res;
            }

            size = fs_fsize(fd);

            if (size == -1) {
                drop_obj(types);
                fs_fclose(fd);
                res = error(ERR_LENGTH, "get: file '%s': invalid size: %d", AS_C8(path), size);
                drop_obj(path);
                return res;
            }

            buf = (str_p)mmap_file(fd, NULL, size, 0, 0);

            if (buf == NULL) {
                drop_obj(types);
                fs_fclose(fd);
                THROW(ERR_IO, "csv: mmap failed");
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
                res = error(ERR_LENGTH, "csv: file '%s': invalid size: %d", AS_C8(path), size);
                drop_obj(path);
                return res;
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
                        THROW(ERR_LENGTH, "csv: file '%s': invalid header (number of fields is less then csv contains)",
                              AS_C8(x[1]));
                    }

                    pos = prev + len;
                    // truncate \r (if any)
                    if ((len > 0) && (pos[-1] == '\r'))
                        pos--;
                }

                AS_SYMBOL(names)[i] = symbols_intern(prev, pos - prev);
                pos++;
                len -= (pos - prev);
            }

            if (i != l) {
                drop_obj(types);
                drop_obj(names);
                fs_fclose(fd);
                mmap_free(buf, size);
                res = error(ERR_LENGTH, "csv: file '%s': invalid header (number of fields is less then csv contains)",
                            AS_C8(path));
                drop_obj(path);
                return res;
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
                drop_obj(cols);
                return res;
            }

            return table(names, cols);
        default:
            THROW(ERR_LENGTH, "csv: expected 1..3 arguments, got %d", n);
    }
}

obj_p ray_parse(obj_p x) {
    if (!x || x->type != TYPE_C8)
        THROW(ERR_TYPE, "parse: expected string");

    return ray_parse_str(0, x, NULL);
}

obj_p ray_eval(obj_p x) {
    if (!x || x->type != TYPE_C8)
        THROW(ERR_TYPE, "eval: expected string");

    return ray_eval_str(x, NULL_OBJ);
}

obj_p ray_load(obj_p x) {
    u64_t flen;
    obj_p file, sym, tab, res;
    lit_p fname;

    if (!x || x->type != TYPE_C8)
        THROW(ERR_TYPE, "load: expected string");

    // table expected
    if (x->len > 1 && AS_C8(x)[x->len - 1] == '/') {
        tab = ray_get(x);
        if (IS_ERROR(tab))
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
    if (IS_ERROR(file))
        return file;

    res = ray_eval_str(file, x);
    drop_obj(file);

    return res;
}

obj_p distinct_syms(obj_p *x, u64_t n) {
    i64_t p;
    u64_t i, j, h, l;
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
        s = cstring_from_str("sym", 3);
        col = ray_concat(path, s);
        v = ray_get(col);
        drop_obj(s);
        drop_obj(col);
    }

    if (IS_ERROR(v))
        return v;

    s = symbol("sym", 3);
    drop_obj(ray_set(s, v));
    drop_obj(s);
    drop_obj(v);

    return NULL_OBJ;
}

obj_p io_set_table_splayed(obj_p path, obj_p table, obj_p symfile) {
    u64_t i, l;
    obj_p res, col, s, p, v, e, cols, sym;

    if (path->type != TYPE_C8)
        THROW(ERR_TYPE, "set: table path must be a string");

    if (table->type != TYPE_TABLE)
        THROW(ERR_TYPE, "set: table must be a table");

    if (path->len < 2 || AS_C8(path)[path->len - 1] != '/')
        THROW(ERR_TYPE, "set: table path must be a directory");

    // save columns schema
    s = cstring_from_str(".d", 2);
    col = ray_concat(path, s);
    res = binary_set(col, AS_LIST(table)[0]);

    drop_obj(s);
    drop_obj(col);

    if (IS_ERROR(res))
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
                // it with sym and enumerate symbol columns over the whole new sym file, preserving ids of the old ones
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
                THROW(ERR_TYPE, "set: symfile must be a string");
        }

        if (IS_ERROR(res))
            return res;

        drop_obj(res);

        s = symbol("sym", 3);
        res = binary_set(s, sym);

        drop_obj(s);

        if (IS_ERROR(res))
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

            if (IS_ERROR(e))
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

        if (IS_ERROR(res))
            return res;

        drop_obj(res);
    }

    return clone_obj(path);
}

obj_p io_get_table_splayed(obj_p path, obj_p symfile) {
    obj_p col, keys, vals, val, s, v;
    u64_t i, l;

    // first try to read columns schema
    s = cstring_from_str(".d", 2);
    col = ray_concat(path, s);
    keys = ray_get(col);
    drop_obj(s);
    drop_obj(col);

    if (IS_ERROR(keys))
        return keys;

    if (keys->type != TYPE_SYMBOL) {
        drop_obj(keys);
        THROW(ERR_TYPE, "get: expected table schema as a symbol vector, got: '%s", type_name(keys->type));
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

        if (IS_ERROR(val)) {
            vals->len = i;
            drop_obj(vals);
            drop_obj(keys);

            return val;
        }

        AS_LIST(vals)[i] = val;
    }

    // read symbol data (if any) if sym is not present in current env
    if (resolve(SYMBOL_SYM) == NULL) {
        v = (symfile->type != TYPE_NULL) ? io_get_symfile(symfile) : io_get_symfile(path);

        if (IS_ERROR(v)) {
            drop_obj(keys);
            drop_obj(vals);
            return v;
        }
    }

    return table(keys, vals);
}