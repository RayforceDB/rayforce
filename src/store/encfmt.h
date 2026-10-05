/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
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
#ifndef RAY_ENCFMT_H
#define RAY_ENCFMT_H

/* Generation-2 column file layout (spec "Format").  Normative:
 *   [32B ray_t header][enc header 64B][directory n_chunks x 64B]
 *   [chunk: 64 x 48B descriptors][payloads] ... [side data] [index region]
 * Every structure is little-endian; generation 2 is defined only for
 * little-endian hosts (RAY_ENC_LE). */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "store/enc.h"

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && \
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#  define RAY_ENC_LE 1
#elif defined(_WIN32)
#  define RAY_ENC_LE 1
#else
#  define RAY_ENC_LE 0
#endif

#define RAY_ENC_GEN             ((uint8_t)2)
#define RAY_ENC_XENC            0x434E4558u   /* aux bytes 0..3 "XENC" */
#define RAY_ENC_XIDX            0x58444958u   /* generation-0 "XIDX" index marker */
#define RAY_ENC_RENC            0x434E4552u   /* enc header magic "RENC" */
#define RAY_ENC_HDR_VERSION     1
#define RAY_ENC_HDR_OFF         32
#define RAY_ENC_DIR_OFF         96
#define RAY_ENC_VECS_PER_CHUNK  64
#define RAY_ENC_DESC_AREA       (RAY_ENC_VECS_PER_CHUNK * 48)
#define RAY_ENC_FLAG_UNIQUE     0x01          /* col_flags bit 0 */
#define RAY_ENC_STAT_ABSENT     0x01          /* directory and descriptor flags bit 0 */
#define RAY_ENC_DESC_TRAILER    0x02          /* descriptor flags bit 1 */
#define RAY_ENC_SIDE_VALUE_DICT 1
#define RAY_ENC_SIDE_STR_DICT   2
#define RAY_ENC_SIDE_FSST_TABLE 3

#define RAY_ENC_GEN_CORRUPT   (-1)
#define RAY_ENC_GEN_VERSION   (-2)

typedef struct ray_enc_hdr_s {
    uint8_t  magic[4];          /*  0 "RENC" */
    uint16_t header_version;    /*  4 */
    uint8_t  index_kind;        /*  6 ray_idx_kind_t */
    uint8_t  col_flags;         /*  7 bit 0 UNIQUE */
    uint32_t rsv0;              /*  8 */
    uint32_t rsv1;              /* 12 */
    uint64_t side_off;          /* 16 */
    uint64_t side_len;          /* 24 */
    uint64_t index_off;         /* 32 */
    uint32_t rsv2;              /* 40 */
    uint32_t rsv3;              /* 44 */
    uint64_t str_pool_bytes;    /* 48 */
    uint64_t end_off;           /* 56 */
} ray_enc_hdr_t;

typedef struct {
    uint64_t offset;            /*  0 */
    uint64_t bytes;             /*  8 */
    int64_t  min;               /* 16 */
    int64_t  max;               /* 24 */
    uint64_t sum_lo;            /* 32 */
    int64_t  sum_hi;            /* 40 */
    uint32_t rsv0;              /* 48 */
    uint32_t null_count;        /* 52 */
    uint32_t flags;             /* 56 */
    uint32_t rsv1;              /* 60 */
} ray_enc_dirent_t;

typedef struct {
    uint8_t  encoding;          /*  0 */
    uint8_t  width;             /*  1 */
    uint16_t n_nulls;           /*  2 */
    uint16_t flags;             /*  4 */
    uint16_t rsv0;              /*  6 */
    uint64_t payload_off;       /*  8 */
    uint64_t payload_bytes;     /* 16 */
    uint32_t rsv1;              /* 24 */
    uint32_t rsv2;              /* 28 */
    int64_t  min;               /* 32 */
    int64_t  max;               /* 40 */
} ray_enc_desc_t;

#define ENC_OFF(T, f, o) _Static_assert(offsetof(T, f) == (o), #T "." #f " offset")
_Static_assert(sizeof(ray_enc_hdr_t) == 64, "enc header is 64 bytes");
ENC_OFF(ray_enc_hdr_t, magic, 0);
ENC_OFF(ray_enc_hdr_t, header_version, 4);
ENC_OFF(ray_enc_hdr_t, index_kind, 6);
ENC_OFF(ray_enc_hdr_t, col_flags, 7);
ENC_OFF(ray_enc_hdr_t, rsv0, 8);
ENC_OFF(ray_enc_hdr_t, rsv1, 12);
ENC_OFF(ray_enc_hdr_t, side_len, 24);
ENC_OFF(ray_enc_hdr_t, rsv2, 40);
ENC_OFF(ray_enc_hdr_t, rsv3, 44);
ENC_OFF(ray_enc_dirent_t, offset, 0);
ENC_OFF(ray_enc_dirent_t, bytes, 8);
ENC_OFF(ray_enc_dirent_t, min, 16);
ENC_OFF(ray_enc_dirent_t, max, 24);
ENC_OFF(ray_enc_dirent_t, sum_lo, 32);
ENC_OFF(ray_enc_dirent_t, sum_hi, 40);
ENC_OFF(ray_enc_dirent_t, rsv0, 48);
ENC_OFF(ray_enc_dirent_t, rsv1, 60);
ENC_OFF(ray_enc_desc_t, encoding, 0);
ENC_OFF(ray_enc_desc_t, width, 1);
ENC_OFF(ray_enc_desc_t, n_nulls, 2);
ENC_OFF(ray_enc_desc_t, flags, 4);
ENC_OFF(ray_enc_desc_t, rsv0, 6);
ENC_OFF(ray_enc_desc_t, payload_bytes, 16);
ENC_OFF(ray_enc_desc_t, rsv1, 24);
ENC_OFF(ray_enc_desc_t, rsv2, 28);
ENC_OFF(ray_enc_desc_t, max, 40);
_Static_assert(RAY_ENC_DESC_AREA == RAY_ENC_VECS_PER_CHUNK * sizeof(ray_enc_desc_t),
               "descriptor area is 64 descriptors");
_Static_assert(offsetof(ray_enc_hdr_t, side_off) == 16, "side_off at 16");
_Static_assert(offsetof(ray_enc_hdr_t, index_off) == 32, "index_off at 32");
_Static_assert(offsetof(ray_enc_hdr_t, str_pool_bytes) == 48, "str_pool_bytes at 48");
_Static_assert(offsetof(ray_enc_hdr_t, end_off) == 56, "end_off at 56");
_Static_assert(sizeof(ray_enc_dirent_t) == 64, "directory entry is 64 bytes");
_Static_assert(offsetof(ray_enc_dirent_t, null_count) == 52, "null_count at 52");
_Static_assert(offsetof(ray_enc_dirent_t, flags) == 56, "flags at 56");
_Static_assert(sizeof(ray_enc_desc_t) == 48, "descriptor is 48 bytes");
_Static_assert(offsetof(ray_enc_desc_t, payload_off) == 8, "payload_off at 8");
_Static_assert(offsetof(ray_enc_desc_t, min) == 32, "desc min at 32");

/* Generation of a file from its first 32 bytes (the pair order, aux[0..3]):
 *   (2, "XENC") -> 2; (0, 0 or "XIDX") -> 0;
 *   (0, anything else) and (2, not "XENC") -> RAY_ENC_GEN_CORRUPT;
 *   any other order byte -> RAY_ENC_GEN_VERSION. */
static inline int ray_col_gen_of(const void* file_header) {
    const uint8_t* p = (const uint8_t*)file_header;
    uint32_t aux0;
    memcpy(&aux0, p, 4);
    uint8_t order = p[17];
    if (order == RAY_ENC_GEN) return aux0 == RAY_ENC_XENC ? 2 : RAY_ENC_GEN_CORRUPT;
    if (order == 0) return (aux0 == 0 || aux0 == RAY_ENC_XIDX) ? 0 : RAY_ENC_GEN_CORRUPT;
    return RAY_ENC_GEN_VERSION;
}

static inline uint64_t ray_enc_pad8(uint64_t x)  { return (x + 7) & ~(uint64_t)7; }
static inline uint64_t ray_enc_pad32(uint64_t x) { return (x + 31) & ~(uint64_t)31; }
static inline int64_t ray_enc_nchunks(int64_t len) {
    return len <= 0 ? 0 : (len + RAY_ENC_CHUNK - 1) / RAY_ENC_CHUNK;
}
static inline int64_t ray_enc_nvecs(int64_t len) {
    return len <= 0 ? 0 : (len + RAY_ENC_VEC - 1) / RAY_ENC_VEC;
}
/* Length of column-global vector `vec`: 1024 except the column's last.
 * Precondition: 0 <= vec < ray_enc_nvecs(len). */
static inline int ray_enc_vec_len(int64_t len, int64_t vec) {
    int64_t rem = len - vec * RAY_ENC_VEC;
    return rem >= RAY_ENC_VEC ? RAY_ENC_VEC : (int)rem;
}

#endif /* RAY_ENCFMT_H */
