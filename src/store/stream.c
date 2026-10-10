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
#include "io/csv.h"      /* ray_csv_hash_upgrade_check: the shared upgrade policy */
#include "store/splay.h" /* ray_splay_hash_column */
#include "core/pool.h"
#include "core/profile.h"
#include "core/runtime.h"
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
    return RAY_OK;
}

static void ray_col_stream_drop_lut(ray_col_stream_t* w) {
    ray_free_raw(w->lut_id); ray_free_raw(w->lut_pos);
    w->lut_id = NULL; w->lut_pos = NULL;
}

/* The runtime id -> position cache, made by the first runtime-domain chunk:
 * 4 MiB of ids and 2 MiB of positions, each a block one order up with its
 * header (8 MB and 4 MB, 12 MB committed, the 4 MiB of ids written here).
 * A writer fed chunks already encoded over its domain never needs it.  Best
 * effort: without it every cell probes the domain. */
static bool ray_col_stream_lut(ray_col_stream_t* w) {
    if (w->lut_id) return true;
    w->lut_id  = (int64_t*)ray_alloc_raw((size_t)COL_STREAM_LUT * sizeof(int64_t));
    w->lut_pos = (uint32_t*)ray_alloc_raw((size_t)COL_STREAM_LUT * sizeof(uint32_t));
    if (!w->lut_id || !w->lut_pos) { ray_col_stream_drop_lut(w); return false; }
    memset(w->lut_id, 0xff, (size_t)COL_STREAM_LUT * sizeof(int64_t));
    return true;
}

ray_err_t ray_col_stream_index_begin(ray_col_stream_t* w, int64_t start_row) {
    if (w->zone || w->dict) return RAY_ERR_DOMAIN;
    if (w->type == RAY_STR) {
        w->dict = (ray_dict_acc_t*)ray_alloc_raw(sizeof(*w->dict));
        if (!w->dict) return RAY_ERR_OOM;
        ray_err_t err = ray_dict_acc_init(w->dict);
        if (err != RAY_OK) { ray_free_raw(w->dict); w->dict = NULL; }
        return err;
    }
    if (!ray_zone_acc_supported(w->type)) return RAY_OK;
    w->zone = (ray_zone_acc_t*)ray_alloc_raw(sizeof(*w->zone));
    if (!w->zone) return RAY_ERR_OOM;
    ray_err_t err = ray_zone_acc_init(w->zone, w->type, start_row);
    if (err != RAY_OK) { ray_free_raw(w->zone); w->zone = NULL; }
    return err;
}

ray_err_t ray_col_stream_index_merge(ray_col_stream_t* dst, ray_col_stream_t* src) {
    if (!src->zone) return RAY_OK;
    ray_err_t err = dst->zone ? ray_zone_acc_merge(dst->zone, src->zone) : RAY_ERR_DOMAIN;
    ray_zone_acc_free(src->zone); ray_free_raw(src->zone); src->zone = NULL;
    return err;
}

static void stream_drop_index(ray_col_stream_t* w) {
    if (w->zone) { ray_zone_acc_free(w->zone); ray_free_raw(w->zone); w->zone = NULL; }
    if (w->dict) { ray_dict_acc_free(w->dict); ray_free_raw(w->dict); w->dict = NULL; }
    if (w->index) { ray_release(w->index); w->index = NULL; }
    w->wants_hash = false;
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
        const char* strings[BATCH]; size_t lengths[BATCH]; int64_t ids[BATCH];
        /* The batch's hashes, then (they are no longer read once the batch
         * is interned) the positions written: one array's room on a worker's
         * stack, as when the hashes were 32 bits wide. */
        union { uint64_t h64[BATCH]; uint32_t h32[BATCH]; uint32_t pos[BATCH]; } u;
        /* One hash a string: all 64 bits for an import domain, which keys on
         * them (ray_sym_domain_intern_batch64, which would otherwise hash
         * the strings again), the low 32 for the others. */
        bool wide = ray_sym_domain_import(w->dom) != NULL;
        for (int64_t off = 0; off < n; off += BATCH) {
            if (ray_interrupted()) return RAY_ERR_CANCEL;
            int64_t count = n-off < BATCH ? n-off : BATCH;
            for (int64_t i = 0; i < count; i++) strings[i] = ray_str_vec_get(col,off+i,&lengths[i]);
            bool ok;
            if (wide) {
                for (int64_t i = 0; i < count; i++) u.h64[i] = ray_hash_bytes(strings[i],lengths[i]);
                ok = ray_sym_domain_intern_batch64(w->dom,count,strings,lengths,u.h64,ids);
            } else {
                for (int64_t i = 0; i < count; i++) u.h32[i] = (uint32_t)ray_hash_bytes(strings[i],lengths[i]);
                ok = ray_sym_domain_intern_batch(w->dom,count,strings,lengths,u.h32,ids);
            }
            if (!ok) return RAY_ERR_OOM;
            for (int64_t i = 0; i < count; i++) {
                if (ids[i] < 0 || (uint64_t)ids[i] >= UINT32_MAX) return RAY_ERR_RANGE;
                u.pos[i] = (uint32_t)ids[i]; if (!ids[i]) w->had_nulls = true;
            }
            if (fwrite(u.pos,sizeof(*u.pos),(size_t)count,w->fp) != (size_t)count) return RAY_ERR_IO;
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
    } else if (w->type == RAY_SYM && ray_sym_vec_domain(col) == w->dom &&
               (col->attrs & RAY_SYM_W_MASK) == RAY_SYM_W32) {
        /* Already encoded over the target domain at the file's width (a
         * converter's own symbol decode): the cells are the positions,
         * written as they are in one request.  HAS_NULLS from a position 0
         * among them, as the arm below decides it (not from the chunk's
         * flag); once found, the later chunks are not scanned. */
        const uint32_t* p = (const uint32_t*)ray_data(col);
        for (int64_t off = 0; off < n && !w->had_nulls; off += 8192) {
            int64_t end = n - off < 8192 ? n : off + 8192;
            uint32_t lo = UINT32_MAX;
            for (int64_t i = off; i < end; i++) lo = p[i] < lo ? p[i] : lo;
            if (!lo) w->had_nulls = true;
        }
        if (n && fwrite(p, sizeof(uint32_t), (size_t)n, w->fp) != (size_t)n)
            return RAY_ERR_IO;
    } else if (w->type == RAY_SYM) {
        /* Encode cells as positions in the target symfile's domain:
         * resolve each cell through the chunk vec's own domain and
         * find-or-append into the target (distinct work rides the
         * write).  The domain is flushed before the column files are
         * committed (close), preserving the sym-first crash ordering.
         * A runtime-domain chunk vec goes through the id -> position
         * cache: only a value's first encounter pays the domain probe. */
        bool direct = ray_sym_vec_domain(col) == w->dom;
        bool cached = !direct && ray_sym_vec_domain(col) == ray_sym_runtime_domain() &&
                      ray_col_stream_lut(w);
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
    if (w->zone && col->type == w->type) {
        ray_err_t err = ray_zone_acc_add(w->zone, col);
        if (err != RAY_OK) return err;
    }
    if (w->dict && col->type == RAY_STR) {
        ray_err_t err = ray_dict_acc_add(w->dict, col);
        if (err != RAY_OK) return err;
    }
    w->rows += n;
    return RAY_OK;
}

ray_err_t ray_col_stream_finish(ray_col_stream_t* w, bool durable) {
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

    /* The index region goes after the payload and the marker into the same
     * header write, so the rename publishes data and index together.  A
     * zone the hash policy wants upgraded is handed back instead: the
     * converter re-reads only that column to build the hash. */
    bool inline_index = false;
    /* The persisted HAS_NULLS bit derives from the payload (col.c #495): a
     * sentinel in any slice, flagged or not, publishes the bit, so the
     * loaded column and the zone agree on every cell. */
    if (w->zone && w->zone->saw_null) w->had_nulls = true;
    if (err == RAY_OK && (w->zone || w->dict)) {
        ray_t* idx = w->zone ? ray_zone_acc_finish(w->zone, w->rows)
                             : ray_dict_acc_finish(w->dict, w->rows);
        ray_free_raw(w->zone); ray_free_raw(w->dict); w->zone = NULL; w->dict = NULL;
        if (idx && RAY_IS_ERR(idx)) { ray_error_free(idx); idx = NULL; }   /* best effort */
        if (idx) {
            const ray_index_t* ix = ray_index_payload(idx);
            if (ix->kind == RAY_IDX_CHUNK_ZONE &&
                ray_csv_hash_upgrade_check(w->type, w->rows, ix)) {
                w->index = idx; w->wants_hash = true;
            } else {
                int64_t payload_end = 0;
                ray_err_t ierr = ray_col_write_index_region(w->fp, ix, &payload_end);
                inline_index = ierr == RAY_OK;
                /* The index is a best-effort accelerator: when its region
                 * cannot be written, cut the file back to the payload and
                 * publish the column without it.  Only a failed cut is an
                 * error, since the file would then be unloadable. */
                if (ierr != RAY_OK && payload_end > 0 &&
                    !ray_col_truncate(w->fp, payload_end))
                    err = RAY_ERR_IO;
                ray_release(idx);
            }
        }
    }

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
         * writes — the loaders validate it.  aux stays zero unless an
         * index region was written above, in which case the stamp sets it. */
        ray_col_stamp_format(&hdr);
        if (inline_index) ray_col_stamp_index(&hdr);
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
    if (err != RAY_OK) { remove(w->tmp_path); stream_drop_index(w); }
    else w->finished = true;
    return err;
}

ray_err_t ray_col_stream_publish(ray_col_stream_t* w, bool durable) {
    if (!w->finished) return RAY_OK;
    w->finished = false;
    ray_err_t err = ray_file_rename(w->tmp_path, w->path);
    if (err == RAY_OK && durable) err = ray_file_sync_dir(w->path);
    if (err != RAY_OK) { remove(w->tmp_path); stream_drop_index(w); }
    return err;
}

ray_err_t ray_col_stream_close(ray_col_stream_t* w, bool durable) {
    ray_err_t err = ray_col_stream_finish(w, durable);
    if (err == RAY_OK) err = ray_col_stream_publish(w, durable);
    return err;
}

typedef struct {
    ray_col_stream_t* w;
    int64_t* col_ns;
    bool durable;
    _Atomic(ray_err_t) err;
} stream_finish_ctx_t;

static void stream_finish_task(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid; (void)end;
    stream_finish_ctx_t* a = (stream_finish_ctx_t*)raw;
    int64_t t0 = a->col_ns ? ray_profile_now_ns() : 0;
    ray_err_t e = ray_col_stream_finish(&a->w[start], a->durable);
    if (a->col_ns) a->col_ns[start] = ray_profile_now_ns() - t0;
    if (e != RAY_OK) {
        ray_err_t ok = RAY_OK;
        atomic_compare_exchange_strong_explicit(&a->err, &ok, e, memory_order_relaxed, memory_order_relaxed);
    }
}

ray_err_t ray_col_stream_close_all(ray_col_stream_t* w, int64_t n, bool durable,
                                   int64_t* col_ns) {
    if (n <= 0) return RAY_OK;
    /* finish touches only the writer's own files and never dispatches, so
     * every column finishes side by side: STR pool merges and dictionary
     * regions, which used to run one after another on the caller. */
    stream_finish_ctx_t ctx = { .w = w, .col_ns = col_ns, .durable = durable, .err = RAY_OK };
    ray_pool_t* pool = ray_pool_get();
    if (ray_pool_par_dispatch_ok(pool, n, 2))
        ray_pool_dispatch_n(pool, stream_finish_task, &ctx, (uint32_t)n);
    else
        for (int64_t c = 0; c < n; c++) stream_finish_task(&ctx, 0, c, c + 1);
    ray_err_t err = ctx.err;
    /* The renames stay serial and in column order (microseconds each). */
    for (int64_t c = 0; c < n; c++) {
        ray_err_t cerr = (err == RAY_OK) ? ray_col_stream_publish(&w[c], durable) : RAY_ERR_IO;
        if (err == RAY_OK && cerr != RAY_OK) err = cerr;
        if (err != RAY_OK) ray_col_stream_abort(&w[c]);
    }
    return err;
}

typedef struct {
    ray_col_stream_t* w;
    const int64_t* cand;
    int64_t base;
    int64_t* col_ns;
} stream_hash_ctx_t;

static void stream_hash_task(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid; (void)end;
    stream_hash_ctx_t* a = (stream_hash_ctx_t*)raw;
    int64_t c = a->cand[a->base + start];
    int64_t t0 = a->col_ns ? ray_profile_now_ns() : 0;
    (void)ray_splay_hash_column(a->w[c].path, a->w[c].index);   /* best effort */
    if (a->col_ns) a->col_ns[c] = ray_profile_now_ns() - t0;
}

/* Peak private memory of one in-flight hash build, per row of the column.
 * Inside a pool task the build takes the serial path of
 * ray_index_attach_hash: the probe table (next_pow2(2n) x 8 B: 16-32 B/row,
 * 21.5 at 100 M rows) lives only during the key pass and is freed before
 * the CSR table of the same size is built, so the two never coexist; rgid,
 * gkeys, offs and rows are 8 B/row each (worst case: every key distinct)
 * and the index stays allocated until its region is written.  That is
 * ~54 B/row on paper; measured as a 58 B/row RSS delta per column in
 * flight (2 M-row I64 columns), which is the figure used here.  The mmap of
 * the column file itself is page cache and not counted. */
#define STREAM_HASH_BYTES_PER_ROW 58
/* Share of the RAM budget the in-flight builds may take together. */
#define STREAM_HASH_RAM_SHARE 4

int64_t ray_col_stream_hash_wave(int64_t ram_limit, int64_t rows, int64_t candidates) {
    int64_t budget = ram_limit / STREAM_HASH_RAM_SHARE;
    int64_t per_col = (rows > 0 ? rows : 1) * STREAM_HASH_BYTES_PER_ROW;
    int64_t wave = budget / per_col;
    if (wave < 1) wave = 1;
    if (wave > candidates) wave = candidates;
    return wave;
}

void ray_col_stream_hash_all(ray_col_stream_t* w, int64_t n, int64_t* col_ns) {
    int64_t* cand = (int64_t*)ray_alloc_raw((size_t)(n > 0 ? n : 1) * sizeof(int64_t));
    int64_t ncand = 0, rows = 0;
    if (cand)
        for (int64_t c = 0; c < n; c++)
            if (w[c].wants_hash) { cand[ncand++] = c; if (w[c].rows > rows) rows = w[c].rows; }
    ray_pool_t* pool = ray_pool_get();
    if (ncand >= 2 && ray_pool_par_dispatch_ok(pool, ncand, 2)) {
        /* Columns in flight at once, from the RAM budget (-m, else physical
         * RAM / cgroup): W = ram_limit / SHARE / (58 B * rows), clamped to
         * [1, ncand].  Each task builds serially, so K columns finish in
         * ceil(K / W) serial build times instead of K parallel-inside builds
         * that scale to ~2x on 24 threads. */
        int64_t wave = ray_col_stream_hash_wave(ray_sys_ram_limit(), rows, ncand);
        for (int64_t base = 0; base < ncand; base += wave) {
            int64_t k = ncand - base < wave ? ncand - base : wave;
            stream_hash_ctx_t ctx = { .w = w, .cand = cand, .base = base, .col_ns = col_ns };
            if (k == 1)
                /* A lone build runs here, outside any dispatch, so
                 * ray_parallel_flag stays down and its builder is parallel. */
                stream_hash_task(&ctx, 0, 0, 1);
            else
                ray_pool_dispatch_n(pool, stream_hash_task, &ctx, (uint32_t)k);
        }
    } else {
        /* one candidate (or no pool): the build is parallel inside */
        for (int64_t i = 0; i < ncand; i++) {
            stream_hash_ctx_t ctx = { .w = w, .cand = cand, .base = i, .col_ns = col_ns };
            stream_hash_task(&ctx, 0, 0, 1);
        }
    }
    /* built (or, OOM on the candidate list, left with their zones): a
     * later call has nothing to do for them */
    for (int64_t c = 0; c < n; c++) w[c].wants_hash = false;
    ray_free_raw(cand);
    for (int64_t c = 0; c < n; c++)
        if (w[c].index) { ray_release(w[c].index); w[c].index = NULL; }
}

void ray_col_stream_hash_one(ray_col_stream_t* w) {
    if (!w->wants_hash) return;
    (void)ray_splay_hash_column(w->path, w->index);   /* best effort */
    w->wants_hash = false;
    if (w->index) { ray_release(w->index); w->index = NULL; }
}

void ray_col_stream_abort(ray_col_stream_t* w) {
    ray_col_stream_drop_lut(w);
    stream_drop_index(w);
    w->finished = false;
    if (w->fp) fclose(w->fp);
    w->fp = NULL;
    if (w->pool_fp) fclose(w->pool_fp);
    w->pool_fp = NULL;
    if (w->tmp_path[0]) remove(w->tmp_path);
    if (w->pool_path[0]) remove(w->pool_path);
}
