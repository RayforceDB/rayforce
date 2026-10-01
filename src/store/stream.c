/* Copyright (c) 2026 Anton Kundenko. MIT license; see LICENSE.
 * Shared by CSV and Parquet native conversion. */
#include "stream.h"
#include "ops/hash.h"
#include "store/col.h"
#include "store/fileio.h"
#include "mem/heap.h"
#include "table/sym.h"
#include "table/domain.h"
#include "vec/str.h"
#include <limits.h>
#include <string.h>

#define COL_STREAM_LUT_BITS 19
#define COL_STREAM_LUT (1u << COL_STREAM_LUT_BITS)

ray_err_t ray_col_stream_open(ray_col_stream_t* w,
                                         const char* dir, int64_t name_id,
                                         int8_t type,
                                         struct ray_sym_domain_s* dom) {
    memset(w, 0, sizeof(*w));
    w->type = type;
    w->attrs = (type == RAY_SYM) ? RAY_SYM_W32 : 0;
    w->dom = dom;
    if (type == RAY_SYM && !dom) return RAY_ERR_IO;

    ray_t* name_atom = ray_sym_str(name_id);
    if (!name_atom) return RAY_ERR_CORRUPT;
    const char* name = ray_str_ptr(name_atom);
    size_t name_len = ray_str_len(name_atom);
    if (name_len == 0 || name[0] == '.' ||
        memchr(name, '/', name_len) || memchr(name, '\\', name_len) ||
        memchr(name, '\0', name_len))
        return RAY_ERR_DOMAIN;

    int n = snprintf(w->path, sizeof(w->path), "%s/%.*s",
                     dir, (int)name_len, name);
    if (n < 0 || (size_t)n >= sizeof(w->path)) return RAY_ERR_RANGE;
    n = snprintf(w->tmp_path, sizeof(w->tmp_path), "%s/.stream-%.*s.col", dir, (int)name_len, name);
    if (n < 0 || (size_t)n >= sizeof(w->tmp_path)) return RAY_ERR_RANGE;
    n = snprintf(w->pool_path, sizeof(w->pool_path), "%s/.stream-%.*s.pool", dir, (int)name_len, name);
    if (n < 0 || (size_t)n >= sizeof(w->pool_path)) return RAY_ERR_RANGE;

    w->fp = fopen(w->tmp_path, "wb+");
    if (!w->fp) return RAY_ERR_IO;
    ray_t zero = {0};
    if (fwrite(&zero, 1, 32, w->fp) != 32) return RAY_ERR_IO;
    if (type == RAY_SYM) {
        /* best effort: without the cache every cell probes the domain */
        w->lut_id  = (int64_t*)ray_alloc_raw((size_t)COL_STREAM_LUT * sizeof(int64_t));
        w->lut_pos = (uint32_t*)ray_alloc_raw((size_t)COL_STREAM_LUT * sizeof(uint32_t));
        if (!w->lut_id || !w->lut_pos) {
            ray_free_raw(w->lut_id); ray_free_raw(w->lut_pos);
            w->lut_id = NULL; w->lut_pos = NULL;
        } else {
            memset(w->lut_id, 0xff, (size_t)COL_STREAM_LUT * sizeof(int64_t));
        }
    }
    return RAY_OK;
}

static void ray_col_stream_drop_lut(ray_col_stream_t* w) {
    ray_free_raw(w->lut_id); ray_free_raw(w->lut_pos);
    w->lut_id = NULL; w->lut_pos = NULL;
}

ray_err_t ray_col_stream_append(ray_col_stream_t* w,
                                           ray_t* col) {
    if (!w->fp || !col || RAY_IS_ERR(col) || (col->type != w->type && !(w->type == RAY_SYM && col->type == RAY_STR)) || (col->attrs & RAY_ATTR_SLICE)) return RAY_ERR_TYPE;
    int64_t n = col->len;
    if (n < 0 || n > INT64_MAX-w->rows) return RAY_ERR_RANGE;

    if (w->type == RAY_SYM && col->type == RAY_STR) {
        /* Import strings directly into the file domain, avoiding a second
         * global runtime dictionary and a runtime-id-to-file-id pass. */
        enum { BATCH = 8192 };
        const char* strings[BATCH]; size_t lengths[BATCH];
        uint32_t hashes[BATCH], positions[BATCH]; int64_t ids[BATCH];
        for (int64_t off = 0; off < n; off += BATCH) {
            if (ray_interrupted()) return RAY_ERR_CANCEL;
            int64_t count = n-off < BATCH ? n-off : BATCH;
            for (int64_t i = 0; i < count; i++) {
                strings[i] = ray_str_vec_get(col,off+i,&lengths[i]);
                hashes[i] = (uint32_t)ray_hash_bytes(strings[i],lengths[i]);
            }
            if (!ray_sym_domain_intern_batch(w->dom,count,strings,lengths,hashes,ids)) return RAY_ERR_OOM;
            for (int64_t i = 0; i < count; i++) {
                if (ids[i] < 0 || (uint64_t)ids[i] >= UINT32_MAX) return RAY_ERR_RANGE;
                positions[i] = (uint32_t)ids[i]; if (!ids[i]) w->had_nulls = true;
            }
            if (fwrite(positions,sizeof(*positions),(size_t)count,w->fp) != (size_t)count) return RAY_ERR_IO;
        }
    } else if (w->type == RAY_STR) {
        uint64_t bytes = col->str_pool ? (uint64_t)col->str_pool->len : 0;
        if (bytes > UINT32_MAX-w->pool_bytes) return RAY_ERR_RANGE;
        const ray_str_t* src = ray_data(col);
        ray_str_t buf[1024];
        for (int64_t off = 0; off < n;) {
            if (ray_interrupted()) return RAY_ERR_CANCEL;
            int64_t count = n-off < 1024 ? n-off : 1024;
            memcpy(buf,src+off,(size_t)count*sizeof(*buf));
            for (int64_t i = 0; i < count; i++) {
                if (buf[i].len > RAY_STR_INLINE_MAX) {
                    if (buf[i].pool_off > bytes || buf[i].len > bytes-buf[i].pool_off) return RAY_ERR_CORRUPT;
                    buf[i].pool_off += (uint32_t)w->pool_bytes;
                }
                if (!buf[i].len) w->had_nulls = true;
            }
            if (fwrite(buf,sizeof(*buf),(size_t)count,w->fp) != (size_t)count) return RAY_ERR_IO;
            off += count;
        }
        if (bytes) {
            if (!w->pool_fp) w->pool_fp = fopen(w->pool_path,"wb+");
            if (!w->pool_fp || fwrite(ray_data(col->str_pool),1,(size_t)bytes,w->pool_fp) != bytes) return RAY_ERR_IO;
        }
        w->pool_bytes += bytes;
    } else if (w->type == RAY_SYM) {
        /* Encode cells as positions in the target symfile's domain:
         * resolve each cell through the chunk vec's own domain and
         * find-or-append into the target (distinct work rides the
         * write).  The domain is flushed before the column files are
         * committed (close), preserving the sym-first crash ordering.
         * A runtime-domain chunk vec goes through the id -> position
         * cache: only a value's first encounter pays the domain probe. */
        bool direct = ray_sym_vec_domain(col) == w->dom;
        bool cached = ray_sym_vec_domain(col) == ray_sym_runtime_domain() && w->lut_id;
        const void* cd = ray_data(col);
        uint32_t buf[8192];
        for (int64_t off = 0; off < n; ) {
            int64_t cnt = n - off;
            if (cnt > (int64_t)(sizeof(buf) / sizeof(buf[0])))
                cnt = (int64_t)(sizeof(buf) / sizeof(buf[0]));
            for (int64_t i = 0; i < cnt; i++) {
                int64_t pos;
                if (direct) {
                    /* Already encoded over the target domain. */
                    pos = ray_read_sym(cd, off + i, RAY_SYM, col->attrs);
                } else if (cached) {
                    int64_t id = ray_read_sym(cd, off + i, RAY_SYM, col->attrs);
                    uint32_t slot = (uint32_t)(((uint64_t)id * 0x9E3779B97F4A7C15ull) >> (64 - COL_STREAM_LUT_BITS));
                    if (w->lut_id[slot] == id) {
                        pos = w->lut_pos[slot];
                    } else {
                        ray_t* s = ray_sym_str(id);
                        if (!s) return RAY_ERR_CORRUPT;
                        pos = ray_sym_domain_intern(w->dom, ray_str_ptr(s), ray_str_len(s));
                        if (pos < 0) return RAY_ERR_OOM;
                        w->lut_id[slot] = id; w->lut_pos[slot] = (uint32_t)pos;
                    }
                } else {
                    ray_t* s = ray_sym_vec_cell(col, off + i);
                    if (!s) return RAY_ERR_CORRUPT;
                    pos = ray_sym_domain_intern(w->dom, ray_str_ptr(s), ray_str_len(s));
                    if (pos < 0) return RAY_ERR_OOM;
                }
                buf[i] = (uint32_t)pos;
                /* Position 0 of any symfile is the empty string (domain.c
                 * enforces that reservation on open), so a re-encoded cell is
                 * the canonical SYM null exactly when its position is 0.
                 * Testing the value WRITTEN keeps the on-disk HAS_NULLS honest
                 * without trusting the chunk vec's attrs — and this arm is why
                 * streamed SYM columns used to lose the bit entirely: only the
                 * non-SYM arm below propagated it (issue #416). */
                if (pos == 0) w->had_nulls = true;
            }
            if (fwrite(buf, sizeof(uint32_t), (size_t)cnt, w->fp) != (size_t)cnt)
                return RAY_ERR_IO;
            off += cnt;
        }
    } else {
        uint8_t esz = ray_sym_elem_size(w->type, 0);
        size_t bytes = (size_t)n * (size_t)esz;
        if (bytes && fwrite(ray_data(col), 1, bytes, w->fp) != bytes)
            return RAY_ERR_IO;
        if (col->attrs & RAY_ATTR_HAS_NULLS) w->had_nulls = true;
    }
    w->rows += n;
    return RAY_OK;
}

ray_err_t ray_col_stream_close(ray_col_stream_t* w, bool durable) {
    ray_col_stream_drop_lut(w);
    if (!w->fp) return RAY_OK;
    ray_err_t err = RAY_OK;

    if (w->type == RAY_STR) {
        ray_t pool_header = {0}; pool_header.type = RAY_U8; pool_header.len = (int64_t)w->pool_bytes;
        if (fwrite(&pool_header,1,32,w->fp) != 32) err = RAY_ERR_IO;
        if (err == RAY_OK && w->pool_fp) {
            uint8_t* buffer = ray_alloc_raw(65536);
            if (!buffer) err = RAY_ERR_OOM;
            else {
                if (fseek(w->pool_fp,0,SEEK_SET)) err = RAY_ERR_IO;
                uint64_t left = w->pool_bytes;
                while (left && err == RAY_OK) {
                    size_t n = left < 65536 ? (size_t)left : 65536;
                    if (ray_interrupted()) err = RAY_ERR_CANCEL;
                    else if (fread(buffer,1,n,w->pool_fp) != n || fwrite(buffer,1,n,w->fp) != n) err = RAY_ERR_IO;
                    left -= n;
                }
                ray_free_raw(buffer);
            }
        }
    }
    if (w->pool_fp) { if (fclose(w->pool_fp) && err == RAY_OK) err = RAY_ERR_IO; w->pool_fp = NULL; }
    if (w->pool_path[0]) remove(w->pool_path);

    if (err == RAY_OK) {
        ray_t hdr = {0};
        hdr.type = w->type;
        hdr.attrs = w->attrs;
        hdr.len = w->rows;
        /* SYM: header rc = the symfile's count (the loader's O(1)
         * fast-reject against the FILE domain). */
        hdr.rc = (w->type == RAY_SYM)
            ? (uint32_t)ray_sym_domain_count(w->dom) : 0;
        if (w->had_nulls) hdr.attrs |= RAY_ATTR_HAS_NULLS;
        /* Stamp the on-disk format major version into `order` so the
         * streamed column file shares the exact identity ray_col_save
         * writes — the loaders validate it.  aux stays zero (it was
         * zero-initialized above) — reserved for postponed index data. */
        ray_col_stamp_format(&hdr);
        if (fseek(w->fp, 0, SEEK_SET) != 0 ||
            fwrite(&hdr, 1, 32, w->fp) != 32)
            err = RAY_ERR_IO;
    }

    if (fclose(w->fp) != 0 && err == RAY_OK) err = RAY_ERR_IO;
    w->fp = NULL;
    if (err == RAY_OK && durable) {
        ray_fd_t fd = ray_file_open(w->tmp_path,RAY_OPEN_READ | RAY_OPEN_WRITE);
        if (fd == RAY_FD_INVALID) err = RAY_ERR_IO;
        else { err = ray_file_sync(fd); ray_file_close(fd); }
    }
    if (err == RAY_OK) err = ray_file_rename(w->tmp_path, w->path);
    if (err == RAY_OK && durable) err = ray_file_sync_dir(w->path);
    if (err != RAY_OK) remove(w->tmp_path);
    return err;
}

void ray_col_stream_abort(ray_col_stream_t* w) {
    ray_col_stream_drop_lut(w);
    if (w->fp) fclose(w->fp);
    w->fp = NULL;
    if (w->pool_fp) fclose(w->pool_fp);
    w->pool_fp = NULL;
    if (w->tmp_path[0]) remove(w->tmp_path);
    if (w->pool_path[0]) remove(w->pool_path);
}
