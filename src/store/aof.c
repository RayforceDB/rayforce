/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

/* Embeddable append-only log — see store/aof.h for the design notes and
 * include/rayforce.h for the public contract. */

#define _GNU_SOURCE

#include "store/aof.h"
#include "store/fileio.h"
#include "mem/sys.h"

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

#define AOF_PATH_MAX   1024
#define AOF_HEADER     8            /* u32 len + u32 crc                    */
#define AOF_SEG_FMT    "%020lld.aof"
#define AOF_SEG_NAMELN 24           /* 20 digits + ".aof"                   */

struct ray_aof_s {
    char    dir[AOF_PATH_MAX];
    FILE*   fp;             /* active tail segment, append mode             */
    int64_t next_lsn;       /* LSN the next append receives                 */
    int64_t seg_bytes;      /* valid bytes in the active segment            */
    int64_t seg_limit;      /* rotation threshold                           */
};

/* ── CRC32 (IEEE reflected, poly 0xEDB88320) ───────────────────────── */

static uint32_t aof_crc_table[256];
static bool     aof_crc_ready = false;

static void aof_crc_init(void) {
    if (aof_crc_ready) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        aof_crc_table[i] = c;
    }
    aof_crc_ready = true;
}

static uint32_t aof_crc32(const uint8_t* p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++)
        c = aof_crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ── Segment enumeration ───────────────────────────────────────────── */

/* Collect segment first-LSNs in `dir`, ascending.  Returns count, or -1
 * on I/O error.  *out is a ray_sys_alloc'd array the caller frees (NULL
 * when count is 0). */
static int64_t aof_segments(const char* dir, int64_t** out) {
    *out = NULL;
    DIR* d = opendir(dir);
    if (!d) return errno == ENOENT ? 0 : -1;

    int64_t  cap = 16, n = 0;
    int64_t* v = ray_sys_alloc((size_t)cap * sizeof(int64_t));
    if (!v) { closedir(d); return -1; }

    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        size_t len = strlen(ent->d_name);
        if (len != AOF_SEG_NAMELN || strcmp(ent->d_name + 20, ".aof") != 0)
            continue;
        long long base;
        if (sscanf(ent->d_name, "%20lld", &base) != 1) continue;
        if (n == cap) {
            cap *= 2;
            int64_t* nv = ray_sys_realloc(v, (size_t)cap * sizeof(int64_t));
            if (!nv) { ray_sys_free(v); closedir(d); return -1; }
            v = nv;
        }
        v[n++] = (int64_t)base;
    }
    closedir(d);

    /* insertion sort — segment counts are small (256MB each) */
    for (int64_t i = 1; i < n; i++) {
        int64_t key = v[i], j = i - 1;
        while (j >= 0 && v[j] > key) { v[j + 1] = v[j]; j--; }
        v[j + 1] = key;
    }
    if (n == 0) { ray_sys_free(v); v = NULL; }
    *out = v;
    return n;
}

static void aof_seg_path(char* buf, const char* dir, int64_t first_lsn) {
    snprintf(buf, AOF_PATH_MAX, "%s/" AOF_SEG_FMT, dir, (long long)first_lsn);
}

/* Scan one segment file.  Delivers each valid record to `cb` (which may
 * be NULL for validate-only).  Sets *out_count to valid records,
 * *out_valid_bytes to the byte offset past the last valid record.
 * Returns RAY_OK unless a hard I/O error occurs (a torn/corrupt record
 * is a clean stop, not an error — the caller decides what it means).
 * *out_stopped is true when the callback asked to stop early. */
static ray_err_t aof_scan_segment(const char* path, int64_t base_lsn,
                                  int64_t from_lsn, ray_aof_scan_cb_t cb,
                                  void* ctx, int64_t* out_count,
                                  int64_t* out_valid_bytes,
                                  int64_t* out_delivered,
                                  bool* out_stopped, bool* out_clean) {
    *out_count = 0;
    *out_valid_bytes = 0;
    *out_stopped = false;
    *out_clean = true;

    FILE* f = fopen(path, "rb");
    if (!f) return RAY_ERR_IO;

    uint8_t  head[AOF_HEADER];
    uint8_t* buf = NULL;
    size_t   buf_cap = 0;
    int64_t  lsn = base_lsn;

    for (;;) {
        size_t got = fread(head, 1, AOF_HEADER, f);
        if (got == 0) break;                       /* clean EOF            */
        if (got < AOF_HEADER) { *out_clean = false; break; }

        uint32_t len, crc;
        memcpy(&len, head, 4);
        memcpy(&crc, head + 4, 4);

        if (len > buf_cap) {
            uint8_t* nb = ray_sys_realloc(buf, len ? len : 1);
            if (!nb) { ray_sys_free(buf); fclose(f); return RAY_ERR_OOM; }
            buf = nb;
            buf_cap = len;
        }
        if (fread(buf, 1, len, f) != len) { *out_clean = false; break; }
        if (aof_crc32(buf, len) != crc)   { *out_clean = false; break; }

        (*out_count)++;
        *out_valid_bytes += AOF_HEADER + (int64_t)len;

        if (cb && lsn >= from_lsn) {
            (*out_delivered)++;
            if (!cb(lsn, buf, (int64_t)len, ctx)) {
                *out_stopped = true;
                break;
            }
        }
        lsn++;
    }
    ray_sys_free(buf);
    fclose(f);
    return RAY_OK;
}

/* ── Public API ────────────────────────────────────────────────────── */

ray_aof_t* ray_aof_open(const char* dir, int64_t segment_limit,
                        ray_err_t* out_err) {
    ray_err_t stub;
    if (!out_err) out_err = &stub;
    *out_err = RAY_OK;
    aof_crc_init();

    if (!dir || strlen(dir) >= AOF_PATH_MAX - AOF_SEG_NAMELN - 2) {
        *out_err = RAY_ERR_DOMAIN;
        return NULL;
    }
    if (ray_mkdir_p(dir) != RAY_OK) { *out_err = RAY_ERR_IO; return NULL; }

    ray_aof_t* log = ray_sys_alloc(sizeof(*log));
    if (!log) { *out_err = RAY_ERR_OOM; return NULL; }
    memset(log, 0, sizeof(*log));
    strcpy(log->dir, dir);
    log->seg_limit = segment_limit > 0 ? segment_limit
                                       : RAY_AOF_DEFAULT_SEGMENT_LIMIT;

    /* Recover the tail segment: count valid records, truncate torn tail. */
    int64_t* segs = NULL;
    int64_t  nseg = aof_segments(dir, &segs);
    if (nseg < 0) { ray_sys_free(log); *out_err = RAY_ERR_IO; return NULL; }

    int64_t tail_base = 0;
    char    path[AOF_PATH_MAX];
    if (nseg > 0) {
        tail_base = segs[nseg - 1];
        aof_seg_path(path, dir, tail_base);
        int64_t count = 0, valid = 0, delivered = 0;
        bool    stopped = false, clean = true;
        ray_err_t err = aof_scan_segment(path, tail_base, INT64_MAX, NULL,
                                         NULL, &count, &valid, &delivered,
                                         &stopped, &clean);
        if (err != RAY_OK) {
            ray_sys_free(segs);
            ray_sys_free(log);
            *out_err = err;
            return NULL;
        }
        if (!clean) {
            /* Torn tail == un-acknowledged appends.  Truncate; the byte
             * offset is exact because valid counts whole records only. */
            if (truncate(path, (off_t)valid) != 0) {
                ray_sys_free(segs);
                ray_sys_free(log);
                *out_err = RAY_ERR_IO;
                return NULL;
            }
        }
        log->next_lsn = tail_base + count;
        log->seg_bytes = valid;
    } else {
        aof_seg_path(path, dir, 0);
        log->next_lsn = 0;
        log->seg_bytes = 0;
    }
    ray_sys_free(segs);

    log->fp = fopen(path, "ab");
    if (!log->fp) { ray_sys_free(log); *out_err = RAY_ERR_IO; return NULL; }
    return log;
}

static ray_err_t aof_rotate(ray_aof_t* log) {
    if (fflush(log->fp) != 0) return RAY_ERR_IO;
    if (ray_file_sync((ray_fd_t)fileno(log->fp)) != RAY_OK) return RAY_ERR_IO;
    if (fclose(log->fp) != 0) { log->fp = NULL; return RAY_ERR_IO; }

    char path[AOF_PATH_MAX];
    aof_seg_path(path, log->dir, log->next_lsn);
    log->fp = fopen(path, "ab");
    if (!log->fp) return RAY_ERR_IO;
    log->seg_bytes = 0;

    /* Make the new directory entry itself durable. */
    return ray_file_sync_dir(log->dir);
}

int64_t ray_aof_append(ray_aof_t* log, const void* payload, int64_t len,
                       ray_err_t* out_err) {
    ray_err_t stub;
    if (!out_err) out_err = &stub;
    *out_err = RAY_OK;

    if (!log || !log->fp || (!payload && len > 0) || len < 0) {
        *out_err = RAY_ERR_DOMAIN;
        return -1;
    }
    if (len > (int64_t)UINT32_MAX) { *out_err = RAY_ERR_LIMIT; return -1; }

    if (log->seg_bytes > 0 &&
        log->seg_bytes + AOF_HEADER + len > log->seg_limit) {
        ray_err_t err = aof_rotate(log);
        if (err != RAY_OK) { *out_err = err; return -1; }
    }

    uint32_t len32 = (uint32_t)len;
    uint32_t crc = aof_crc32(payload, (size_t)len);
    if (fwrite(&len32, 1, 4, log->fp) != 4 ||
        fwrite(&crc, 1, 4, log->fp) != 4 ||
        (len > 0 && fwrite(payload, 1, (size_t)len, log->fp) != (size_t)len)) {
        *out_err = RAY_ERR_IO;
        return -1;
    }
    log->seg_bytes += AOF_HEADER + len;
    return log->next_lsn++;
}

ray_err_t ray_aof_commit(ray_aof_t* log) {
    if (!log || !log->fp) return RAY_ERR_DOMAIN;
    if (fflush(log->fp) != 0) return RAY_ERR_IO;
    return ray_file_sync((ray_fd_t)fileno(log->fp));
}

int64_t ray_aof_next_lsn(const ray_aof_t* log) {
    return log ? log->next_lsn : -1;
}

int64_t ray_aof_scan(const char* dir, int64_t from_lsn, ray_aof_scan_cb_t cb,
                     void* ctx, ray_err_t* out_err) {
    ray_err_t stub;
    if (!out_err) out_err = &stub;
    *out_err = RAY_OK;
    aof_crc_init();

    if (!dir || !cb) { *out_err = RAY_ERR_DOMAIN; return -1; }

    int64_t* segs = NULL;
    int64_t  nseg = aof_segments(dir, &segs);
    if (nseg < 0) { *out_err = RAY_ERR_IO; return -1; }

    int64_t delivered = 0;
    char    path[AOF_PATH_MAX];
    for (int64_t i = 0; i < nseg; i++) {
        /* Skip segments that end before from_lsn: the next segment's
         * base is the first LSN this one does not contain. */
        if (i + 1 < nseg && segs[i + 1] <= from_lsn) continue;

        aof_seg_path(path, dir, segs[i]);
        int64_t count = 0, valid = 0;
        bool    stopped = false, clean = true;
        ray_err_t err = aof_scan_segment(path, segs[i], from_lsn, cb, ctx,
                                         &count, &valid, &delivered,
                                         &stopped, &clean);
        if (err != RAY_OK) { ray_sys_free(segs); *out_err = err; return -1; }
        if (stopped) break;
        if (!clean && i + 1 < nseg) {
            /* A bad record in the interior of the log is corruption, not
             * an uncommitted tail. */
            ray_sys_free(segs);
            *out_err = RAY_ERR_CORRUPT;
            return -1;
        }
        if (!clean) break; /* tail segment: committed prefix ends here */
    }
    ray_sys_free(segs);
    return delivered;
}

ray_err_t ray_aof_close(ray_aof_t* log) {
    if (!log) return RAY_ERR_DOMAIN;
    ray_err_t err = RAY_OK;
    if (log->fp) {
        if (fflush(log->fp) != 0) err = RAY_ERR_IO;
        else err = ray_file_sync((ray_fd_t)fileno(log->fp));
        if (fclose(log->fp) != 0 && err == RAY_OK) err = RAY_ERR_IO;
    }
    ray_sys_free(log);
    return err;
}
