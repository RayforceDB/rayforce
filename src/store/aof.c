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
#include "store/fileio.h"
#include "mem/sys.h"

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

#define AOF_PATH_MAX    1024
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

/* ── CRC32 (IEEE reflected, poly 0xEDB88320), compile-time table ──────
 * A constant table instead of lazy init: ray_aof_open and ray_aof_scan
 * may run concurrently on first use, and a racy one-time init could let
 * a thread CRC against a half-written table — worst case reporting
 * committed data corrupt during recovery on a weak-memory machine. */

static const uint32_t AOF_CRC_TABLE[256] = {
    0x00000000u, 0x77073096u, 0xEE0E612Cu, 0x990951BAu, 0x076DC419u, 0x706AF48Fu, 0xE963A535u, 0x9E6495A3u,
    0x0EDB8832u, 0x79DCB8A4u, 0xE0D5E91Eu, 0x97D2D988u, 0x09B64C2Bu, 0x7EB17CBDu, 0xE7B82D07u, 0x90BF1D91u,
    0x1DB71064u, 0x6AB020F2u, 0xF3B97148u, 0x84BE41DEu, 0x1ADAD47Du, 0x6DDDE4EBu, 0xF4D4B551u, 0x83D385C7u,
    0x136C9856u, 0x646BA8C0u, 0xFD62F97Au, 0x8A65C9ECu, 0x14015C4Fu, 0x63066CD9u, 0xFA0F3D63u, 0x8D080DF5u,
    0x3B6E20C8u, 0x4C69105Eu, 0xD56041E4u, 0xA2677172u, 0x3C03E4D1u, 0x4B04D447u, 0xD20D85FDu, 0xA50AB56Bu,
    0x35B5A8FAu, 0x42B2986Cu, 0xDBBBC9D6u, 0xACBCF940u, 0x32D86CE3u, 0x45DF5C75u, 0xDCD60DCFu, 0xABD13D59u,
    0x26D930ACu, 0x51DE003Au, 0xC8D75180u, 0xBFD06116u, 0x21B4F4B5u, 0x56B3C423u, 0xCFBA9599u, 0xB8BDA50Fu,
    0x2802B89Eu, 0x5F058808u, 0xC60CD9B2u, 0xB10BE924u, 0x2F6F7C87u, 0x58684C11u, 0xC1611DABu, 0xB6662D3Du,
    0x76DC4190u, 0x01DB7106u, 0x98D220BCu, 0xEFD5102Au, 0x71B18589u, 0x06B6B51Fu, 0x9FBFE4A5u, 0xE8B8D433u,
    0x7807C9A2u, 0x0F00F934u, 0x9609A88Eu, 0xE10E9818u, 0x7F6A0DBBu, 0x086D3D2Du, 0x91646C97u, 0xE6635C01u,
    0x6B6B51F4u, 0x1C6C6162u, 0x856530D8u, 0xF262004Eu, 0x6C0695EDu, 0x1B01A57Bu, 0x8208F4C1u, 0xF50FC457u,
    0x65B0D9C6u, 0x12B7E950u, 0x8BBEB8EAu, 0xFCB9887Cu, 0x62DD1DDFu, 0x15DA2D49u, 0x8CD37CF3u, 0xFBD44C65u,
    0x4DB26158u, 0x3AB551CEu, 0xA3BC0074u, 0xD4BB30E2u, 0x4ADFA541u, 0x3DD895D7u, 0xA4D1C46Du, 0xD3D6F4FBu,
    0x4369E96Au, 0x346ED9FCu, 0xAD678846u, 0xDA60B8D0u, 0x44042D73u, 0x33031DE5u, 0xAA0A4C5Fu, 0xDD0D7CC9u,
    0x5005713Cu, 0x270241AAu, 0xBE0B1010u, 0xC90C2086u, 0x5768B525u, 0x206F85B3u, 0xB966D409u, 0xCE61E49Fu,
    0x5EDEF90Eu, 0x29D9C998u, 0xB0D09822u, 0xC7D7A8B4u, 0x59B33D17u, 0x2EB40D81u, 0xB7BD5C3Bu, 0xC0BA6CADu,
    0xEDB88320u, 0x9ABFB3B6u, 0x03B6E20Cu, 0x74B1D29Au, 0xEAD54739u, 0x9DD277AFu, 0x04DB2615u, 0x73DC1683u,
    0xE3630B12u, 0x94643B84u, 0x0D6D6A3Eu, 0x7A6A5AA8u, 0xE40ECF0Bu, 0x9309FF9Du, 0x0A00AE27u, 0x7D079EB1u,
    0xF00F9344u, 0x8708A3D2u, 0x1E01F268u, 0x6906C2FEu, 0xF762575Du, 0x806567CBu, 0x196C3671u, 0x6E6B06E7u,
    0xFED41B76u, 0x89D32BE0u, 0x10DA7A5Au, 0x67DD4ACCu, 0xF9B9DF6Fu, 0x8EBEEFF9u, 0x17B7BE43u, 0x60B08ED5u,
    0xD6D6A3E8u, 0xA1D1937Eu, 0x38D8C2C4u, 0x4FDFF252u, 0xD1BB67F1u, 0xA6BC5767u, 0x3FB506DDu, 0x48B2364Bu,
    0xD80D2BDAu, 0xAF0A1B4Cu, 0x36034AF6u, 0x41047A60u, 0xDF60EFC3u, 0xA867DF55u, 0x316E8EEFu, 0x4669BE79u,
    0xCB61B38Cu, 0xBC66831Au, 0x256FD2A0u, 0x5268E236u, 0xCC0C7795u, 0xBB0B4703u, 0x220216B9u, 0x5505262Fu,
    0xC5BA3BBEu, 0xB2BD0B28u, 0x2BB45A92u, 0x5CB36A04u, 0xC2D7FFA7u, 0xB5D0CF31u, 0x2CD99E8Bu, 0x5BDEAE1Du,
    0x9B64C2B0u, 0xEC63F226u, 0x756AA39Cu, 0x026D930Au, 0x9C0906A9u, 0xEB0E363Fu, 0x72076785u, 0x05005713u,
    0x95BF4A82u, 0xE2B87A14u, 0x7BB12BAEu, 0x0CB61B38u, 0x92D28E9Bu, 0xE5D5BE0Du, 0x7CDCEFB7u, 0x0BDBDF21u,
    0x86D3D2D4u, 0xF1D4E242u, 0x68DDB3F8u, 0x1FDA836Eu, 0x81BE16CDu, 0xF6B9265Bu, 0x6FB077E1u, 0x18B74777u,
    0x88085AE6u, 0xFF0F6A70u, 0x66063BCAu, 0x11010B5Cu, 0x8F659EFFu, 0xF862AE69u, 0x616BFFD3u, 0x166CCF45u,
    0xA00AE278u, 0xD70DD2EEu, 0x4E048354u, 0x3903B3C2u, 0xA7672661u, 0xD06016F7u, 0x4969474Du, 0x3E6E77DBu,
    0xAED16A4Au, 0xD9D65ADCu, 0x40DF0B66u, 0x37D83BF0u, 0xA9BCAE53u, 0xDEBB9EC5u, 0x47B2CF7Fu, 0x30B5FFE9u,
    0xBDBDF21Cu, 0xCABAC28Au, 0x53B39330u, 0x24B4A3A6u, 0xBAD03605u, 0xCDD70693u, 0x54DE5729u, 0x23D967BFu,
    0xB3667A2Eu, 0xC4614AB8u, 0x5D681B02u, 0x2A6F2B94u, 0xB40BBE37u, 0xC30C8EA1u, 0x5A05DF1Bu, 0x2D02EF8Du,
};

static uint32_t aof_crc32(uint32_t seed, const uint8_t* p, size_t n) {
    uint32_t c = seed ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++)
        c = AOF_CRC_TABLE[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

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
    uint32_t c = aof_crc32(0, len_le, 4);
    return aof_crc32(c, payload, len);
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
    snprintf(buf, AOF_PATH_MAX, "%s/" AOF_SEG_FMT, dir, (long long)first_lsn);
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

    char path[AOF_PATH_MAX];
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
    if (ray_file_sync((ray_fd_t)fileno(log->fp)) != RAY_OK) return RAY_ERR_IO;
    if (fclose(log->fp) != 0) { log->fp = NULL; return RAY_ERR_IO; }

    char path[AOF_PATH_MAX];
    aof_seg_path(path, log->dir, log->next_lsn);
    log->fp = fopen(path, "ab");
    if (!log->fp) return RAY_ERR_IO;
    log->seg_bytes = 0;
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

    if (!dir || !cb) { *out_err = RAY_ERR_DOMAIN; return -1; }

    int64_t* segs = NULL;
    int64_t  nseg = aof_segments(dir, &segs);
    if (nseg < 0) { *out_err = RAY_ERR_IO; return -1; }

    int64_t delivered = 0;
    char    path[AOF_PATH_MAX];
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
