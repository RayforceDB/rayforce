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
 * include/rayforce.h for the public contract.
 *
 * The commit boundary is IN the stream: ray_aof_commit() writes a commit
 * frame (a sentinel header) before flush+fsync.  Scans deliver only
 * records that precede the last valid commit frame, so "a scan observes
 * the committed prefix" holds by construction — regardless of what the
 * stdio buffer, the page cache, or a crash did to the uncommitted
 * suffix.  Recovery truncates strictly AFTER the last commit frame
 * (only bytes no reader was ever allowed to observe), so an LSN, once
 * observable, is never reissued for different data.  A CRC failure
 * BEFORE the last commit frame is damage to committed data and fails
 * hard with RAY_ERR_CORRUPT — recovery never silently drops
 * acknowledged records. */

#define _GNU_SOURCE

#include "store/aof.h"
#include "core/crc32.h"
#include "store/fileio.h"
#include "mem/sys.h"

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

/* ray_file_sync takes the platform handle: the descriptor itself on POSIX,
 * the underlying HANDLE on Windows. */
#ifdef RAY_OS_WINDOWS
#include <io.h>
#define AOF_FP_HANDLE(fp) ((ray_fd_t)_get_osfhandle(fileno(fp)))
#else
#define AOF_FP_HANDLE(fp) ((ray_fd_t)fileno(fp))
#endif

#define AOF_PATH_MAX    1024
/* Segment-path buffers are sized past the worst case (dir + '/' + 24-char
 * segment name) so gcc's -Wformat-truncation can prove snprintf fits even
 * without seeing the dir-length guard in ray_aof_open. */
#define AOF_SEGPATH_MAX (AOF_PATH_MAX + 32)
#define AOF_HEADER      8            /* u32 len + u32 crc                   */
#define AOF_SEG_FMT     "%020lld.aof"
#define AOF_SEG_NAMELN  24           /* 20 digits + ".aof"                  */

/* Commit frame: len field = AOF_FRAME_LEN, crc field = AOF_FRAME_MAGIC
 * (CRC32 of the ASCII tag "ray-aof-commit-frame" — a fixed, non-trivial
 * bit pattern so an all-ones or all-zeros header never reads as a valid
 * frame).  User payloads are capped at AOF_LEN_MAX, below the sentinel. */
#define AOF_FRAME_LEN   0xFFFFFFFFu
#define AOF_FRAME_MAGIC 0xB0AD63C9u
#define AOF_LEN_MAX     0xFFFFFFFEu

struct ray_aof_s {
    char    dir[AOF_PATH_MAX];
    FILE*   fp;             /* active tail segment, append mode             */
    int64_t next_lsn;       /* LSN the next append receives                 */
    int64_t seg_bytes;      /* bytes in the active segment                  */
    int64_t seg_limit;      /* rotation threshold                           */
    bool    dirty;          /* appends since the last commit frame          */
};

/* ── Explicit little-endian framing (the header documents LE; the code
 *    must deliver LE on every architecture, not native order) ────────── */

static void aof_put_u32le(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t aof_get_u32le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* Record CRC covers the length prefix AND the payload, so a bit-flipped
 * length cannot masquerade as a huge valid record during recovery. */
static uint32_t aof_record_crc(uint32_t len, const uint8_t* payload) {
    uint8_t len_le[4];
    aof_put_u32le(len_le, len);
    uint32_t c = ray_crc32(0, len_le, 4);
    return ray_crc32(c, payload, len);
}

/* ── Segment enumeration ───────────────────────────────────────────── */

static bool aof_seg_name_valid(const char* name) {
    for (int i = 0; i < 20; i++)
        if (name[i] < '0' || name[i] > '9') return false;
    return strcmp(name + 20, ".aof") == 0;
}

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
        if (strlen(ent->d_name) != AOF_SEG_NAMELN) continue;
        if (!aof_seg_name_valid(ent->d_name)) continue;
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
    snprintf(buf, AOF_SEGPATH_MAX, "%s/" AOF_SEG_FMT, dir, (long long)first_lsn);
}

/* ── Segment walk ──────────────────────────────────────────────────────
 *
 * Pass 1 (aof_seg_survey): find the committed boundary — the byte offset
 * just past the LAST valid commit frame — and whether any record BEFORE
 * that boundary fails its CRC (damage to committed data).  After a CRC
 * failure the walk resyncs at the failed record's stated end (the length
 * prefix is inside the CRC, so this is best-effort); if a later valid
 * frame is then found, the failure sat inside the committed prefix.
 *
 * Pass 2 (aof_seg_deliver): re-walk to the boundary and deliver data
 * records.  Records below the boundary were CRC-verified in pass 1. */

typedef struct {
    int64_t committed_end;      /* bytes: offset past last valid frame     */
    int64_t committed_records;  /* data records before committed_end       */
    bool    corrupt_committed;  /* CRC failure below committed_end         */
    int64_t file_size;          /* bytes walked (== file size on clean)    */
} aof_survey_t;

static ray_err_t aof_seg_survey(const char* path, aof_survey_t* s) {
    memset(s, 0, sizeof(*s));
    FILE* f = fopen(path, "rb");
    if (!f) return RAY_ERR_IO;

    uint8_t  head[AOF_HEADER];
    uint8_t* buf = NULL;
    size_t   buf_cap = 0;
    int64_t  offset = 0, records = 0;
    int64_t  first_bad = -1; /* offset of first CRC-failed record          */

    for (;;) {
        size_t got = fread(head, 1, AOF_HEADER, f);
        if (got < AOF_HEADER) { s->file_size = offset + (int64_t)got; break; }
        uint32_t len = aof_get_u32le(head);
        uint32_t crc = aof_get_u32le(head + 4);

        if (len == AOF_FRAME_LEN) {
            if (crc != AOF_FRAME_MAGIC) { s->file_size = offset + AOF_HEADER; break; }
            offset += AOF_HEADER;
            s->committed_end = offset;
            s->committed_records = records;
            s->file_size = offset;
            continue;
        }
        if (len > AOF_LEN_MAX) { s->file_size = offset + AOF_HEADER; break; }

        if (len > buf_cap) {
            uint8_t* nb = ray_sys_realloc(buf, len ? len : 1);
            if (!nb) { ray_sys_free(buf); fclose(f); return RAY_ERR_OOM; }
            buf = nb;
            buf_cap = len;
        }
        size_t pl = fread(buf, 1, len, f);
        if (pl < len) { s->file_size = offset + AOF_HEADER + (int64_t)pl; break; }
        if (aof_record_crc(len, buf) != crc && first_bad < 0)
            first_bad = offset;

        offset += AOF_HEADER + (int64_t)len;
        records++;
        s->file_size = offset;
    }
    ray_sys_free(buf);
    fclose(f);

    if (first_bad >= 0 && first_bad < s->committed_end)
        s->corrupt_committed = true;
    return RAY_OK;
}

static ray_err_t aof_seg_deliver(const char* path, int64_t base_lsn,
                                 int64_t boundary, int64_t from_lsn,
                                 ray_aof_scan_cb_t cb, void* ctx,
                                 int64_t* delivered, bool* stopped) {
    *stopped = false;
    FILE* f = fopen(path, "rb");
    if (!f) return RAY_ERR_IO;

    uint8_t  head[AOF_HEADER];
    uint8_t* buf = NULL;
    size_t   buf_cap = 0;
    int64_t  offset = 0, lsn = base_lsn;

    while (offset < boundary) {
        if (fread(head, 1, AOF_HEADER, f) != AOF_HEADER) break;
        uint32_t len = aof_get_u32le(head);
        offset += AOF_HEADER;
        if (len == AOF_FRAME_LEN) continue;

        if (len > buf_cap) {
            uint8_t* nb = ray_sys_realloc(buf, len ? len : 1);
            if (!nb) { ray_sys_free(buf); fclose(f); return RAY_ERR_OOM; }
            buf = nb;
            buf_cap = len;
        }
        if (fread(buf, 1, len, f) != len) break;
        offset += (int64_t)len;

        if (lsn >= from_lsn) {
            (*delivered)++;
            if (!cb(lsn, buf, (int64_t)len, ctx)) { *stopped = true; break; }
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

    int64_t* segs = NULL;
    int64_t  nseg = aof_segments(dir, &segs);
    if (nseg < 0) { ray_sys_free(log); *out_err = RAY_ERR_IO; return NULL; }

    char path[AOF_SEGPATH_MAX];
    aof_seg_path(path, dir, 0);
    if (nseg > 0) {
        int64_t tail_base = segs[nseg - 1];
        aof_seg_path(path, dir, tail_base);
        aof_survey_t s;
        ray_err_t err = aof_seg_survey(path, &s);
        if (err == RAY_OK && s.corrupt_committed) {
            /* Damage BELOW the commit boundary is damage to acknowledged
             * data.  Never silently truncate acknowledged records. */
            err = RAY_ERR_CORRUPT;
        }
        if (err == RAY_OK && s.file_size > s.committed_end &&
            truncate(path, (off_t)s.committed_end) != 0) {
            err = RAY_ERR_IO;
        }
        if (err != RAY_OK) {
            ray_sys_free(segs);
            ray_sys_free(log);
            *out_err = err;
            return NULL;
        }
        /* The truncated suffix was never observable (scans stop at the
         * commit boundary), so reusing its LSNs is safe by construction. */
        log->next_lsn = tail_base + s.committed_records;
        log->seg_bytes = s.committed_end;
    }
    ray_sys_free(segs);

    log->fp = fopen(path, "ab");
    if (!log->fp) { ray_sys_free(log); *out_err = RAY_ERR_IO; return NULL; }
    /* A brand-new log just created its first segment file; fsync log->dir
     * (via the segment path — see aof_rotate) so the directory entry is
     * durable before the first commit can acknowledge data. */
    if (nseg == 0) {
        ray_err_t derr = ray_file_sync_dir(path);
        if (derr != RAY_OK) { fclose(log->fp); ray_sys_free(log); *out_err = derr; return NULL; }
    }
    return log;
}

static ray_err_t aof_write_frame(ray_aof_t* log) {
    uint8_t head[AOF_HEADER];
    aof_put_u32le(head, AOF_FRAME_LEN);
    aof_put_u32le(head + 4, AOF_FRAME_MAGIC);
    if (fwrite(head, 1, AOF_HEADER, log->fp) != AOF_HEADER) return RAY_ERR_IO;
    log->seg_bytes += AOF_HEADER;
    log->dirty = false;
    return RAY_OK;
}

/* Rotation is a commit point: frame (if dirty) + flush + fsync the old
 * segment, then open the next one and fsync the directory entry. */
static ray_err_t aof_rotate(ray_aof_t* log) {
    if (log->dirty) {
        ray_err_t err = aof_write_frame(log);
        if (err != RAY_OK) return err;
    }
    if (fflush(log->fp) != 0) return RAY_ERR_IO;
    if (ray_file_sync(AOF_FP_HANDLE(log->fp)) != RAY_OK) return RAY_ERR_IO;
    if (fclose(log->fp) != 0) { log->fp = NULL; return RAY_ERR_IO; }

    char path[AOF_SEGPATH_MAX];
    aof_seg_path(path, log->dir, log->next_lsn);
    log->fp = fopen(path, "ab");
    if (!log->fp) return RAY_ERR_IO;
    log->seg_bytes = 0;
    /* fsync the segment's CONTAINING directory (= log->dir) so the new
     * segment's directory entry is durable — ray_file_sync_dir strips the
     * last component, so pass the segment path, not log->dir (which would
     * fsync log->dir's parent).  Matches sym.c/col.c/journal.c usage. */
    return ray_file_sync_dir(path);
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
    if (len > (int64_t)AOF_LEN_MAX) { *out_err = RAY_ERR_LIMIT; return -1; }

    if (log->seg_bytes > 0 &&
        log->seg_bytes + AOF_HEADER + len > log->seg_limit) {
        ray_err_t err = aof_rotate(log);
        if (err != RAY_OK) { *out_err = err; return -1; }
    }

    uint8_t head[AOF_HEADER];
    aof_put_u32le(head, (uint32_t)len);
    aof_put_u32le(head + 4, aof_record_crc((uint32_t)len, payload));
    if (fwrite(head, 1, AOF_HEADER, log->fp) != AOF_HEADER ||
        (len > 0 && fwrite(payload, 1, (size_t)len, log->fp) != (size_t)len)) {
        *out_err = RAY_ERR_IO;
        return -1;
    }
    log->seg_bytes += AOF_HEADER + len;
    log->dirty = true;
    return log->next_lsn++;
}

ray_err_t ray_aof_commit(ray_aof_t* log) {
    if (!log || !log->fp) return RAY_ERR_DOMAIN;
    if (!log->dirty) return RAY_OK; /* idempotent: nothing new to commit */
    ray_err_t err = aof_write_frame(log);
    if (err != RAY_OK) return err;
    if (fflush(log->fp) != 0) return RAY_ERR_IO;
    return ray_file_sync(AOF_FP_HANDLE(log->fp));
}

int64_t ray_aof_next_lsn(const ray_aof_t* log) {
    return log ? log->next_lsn : -1;
}

int64_t ray_aof_scan(const char* dir, int64_t from_lsn, ray_aof_scan_cb_t cb,
                     void* ctx, ray_err_t* out_err) {
    ray_err_t stub;
    if (!out_err) out_err = &stub;
    *out_err = RAY_OK;

    if (!dir || !cb) { *out_err = RAY_ERR_DOMAIN; return -1; }

    int64_t* segs = NULL;
    int64_t  nseg = aof_segments(dir, &segs);
    if (nseg < 0) { *out_err = RAY_ERR_IO; return -1; }

    int64_t delivered = 0;
    char    path[AOF_SEGPATH_MAX];
    for (int64_t i = 0; i < nseg; i++) {
        if (i + 1 < nseg && segs[i + 1] <= from_lsn) continue;

        aof_seg_path(path, dir, segs[i]);
        aof_survey_t s;
        ray_err_t err = aof_seg_survey(path, &s);
        if (err != RAY_OK) { ray_sys_free(segs); *out_err = err; return -1; }
        if (s.corrupt_committed ||
            (i + 1 < nseg && s.committed_end != s.file_size)) {
            /* CRC failure below the commit boundary, or trailing bytes in
             * a sealed (non-tail) segment: committed data is damaged. */
            ray_sys_free(segs);
            *out_err = RAY_ERR_CORRUPT;
            return -1;
        }
        bool stopped = false;
        err = aof_seg_deliver(path, segs[i], s.committed_end, from_lsn, cb,
                              ctx, &delivered, &stopped);
        if (err != RAY_OK) { ray_sys_free(segs); *out_err = err; return -1; }
        if (stopped) break;
    }
    ray_sys_free(segs);
    return delivered;
}

ray_err_t ray_aof_close(ray_aof_t* log) {
    if (!log) return RAY_ERR_DOMAIN;
    ray_err_t err = RAY_OK;
    if (log->fp) {
        err = ray_aof_commit(log);
        if (fclose(log->fp) != 0 && err == RAY_OK) err = RAY_ERR_IO;
    }
    ray_sys_free(log);
    return err;
}
