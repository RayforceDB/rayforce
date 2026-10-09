/* Copyright (c) 2026 Anton Kundenko. MIT license; see LICENSE.
 * Native Parquet reader. Format references (not implementation dependencies):
 * parquet.apache.org/docs/file-format, Apache Thrift Compact Protocol, and
 * google/snappy format_description.txt. No third-party code or libraries.
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "parquet.h"
#include "core/platform.h"
#include "core/pool.h"
#include "core/crc32.h"
#include "core/profile.h"
#include "mem/heap.h"
#include "mem/sys.h"        /* ray_sys_free: the grouped import's redo list */
#include "table/sym.h"
#include "table/domain.h"
#include "table/symimp.h"   /* ray_symimp_stats_t: RAY_CSV_TRACE */
#include "table/symgrp.h"   /* the grouped symbol import */
#include "vec/str.h"
#include "vec/vec.h"
#include "ops/idxop.h"
#include "ops/hash.h"
#include "ops/glob.h"
#include "ops/agg_registry.h"
#include "store/splay.h"
#include "store/stream.h"
#include "store/col.h"
#include "store/fileio.h"
#include "lang/eval.h"
#include "lang/internal.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#ifndef RAY_OS_WINDOWS
#include <unistd.h>
#endif
#if defined(__linux__)
#include <sys/resource.h>   /* getrusage: the grouped import's trace */
#endif
#if defined(__SSE2__)
#include <emmintrin.h>
#endif

#define PQ_BATCH 65536
#define PQ_MAX_PART_ROWS 1048576
#define PQ_MAX_COLS 4096
#define PQ_MAX_PAGE (64u * 1024u * 1024u)
/* Decoded STR batches: the reader sizes each row group's batches to keep a
 * column's string pool near PQ_POOL_TARGET (pq_group_batch); PQ_POOL_MAX is
 * the pool's real capacity (32-bit offsets), the backstop when a group's
 * metadata underestimates its strings. */
#define PQ_POOL_TARGET (64u * 1024u * 1024u)
#define PQ_POOL_MAX    ((uint64_t)UINT32_MAX)
#define PQ_MAX_FOOTER (64u * 1024u * 1024u)
#define PQ_EPOCH_DAYS 10957
#define PQ_EPOCH_NS INT64_C(946684800000000000)

/* A compact-protocol value is a borrowed, bounded span into the mapping. */
typedef struct { const uint8_t *p, *end; uint8_t type; } pq_span;
typedef struct { const uint8_t *p, *end; bool bad; } pq_cur;
static uint32_t pq_u32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static uint64_t pq_u64(const uint8_t* p) { return pq_u32(p) | (uint64_t)pq_u32(p+4)<<32; }
static uint8_t pq_byte(pq_cur* c) {
    if (c->p == c->end) { c->bad = true; return 0; }
    return *c->p++;
}
static bool pq_take(pq_cur* c, uint64_t n, const uint8_t** out) {
    if (n > (uint64_t)(c->end - c->p)) { c->bad = true; return false; }
    if (out) *out = c->p;
    c->p += (size_t)n;
    return true;
}
static uint64_t pq_var(pq_cur* c) {
    uint64_t v = 0;
    for (unsigned s = 0; s < 64; s += 7) {
        uint8_t b = pq_byte(c);
        if (c->bad || (s == 63 && b > 1)) { c->bad = true; return 0; }
        v |= (uint64_t)(b & 127) << s;
        if (!(b & 128)) return v;
    }
    c->bad = true; return 0;
}
static int64_t pq_int(pq_cur* c) {
    uint64_t u = pq_var(c);
    return (int64_t)(u >> 1) ^ -(int64_t)(u & 1);
}
static uint64_t pq_list(pq_cur* c, uint8_t* type) {
    uint8_t h = pq_byte(c); *type = h & 15;
    uint64_t n = h >> 4;
    if (n == 15) n = pq_var(c);
    /* Every collection element occupies at least one byte. */
    if (!*type || *type > 12 || n > (uint64_t)(c->end-c->p)) c->bad = true;
    return n;
}
static bool pq_skip(pq_cur* c, uint8_t t, unsigned depth, bool collection) {
    if (depth > 32 || c->bad) { c->bad = true; return false; }
    switch (t) {
    case 1: case 2:
        if (collection) { uint8_t b = pq_byte(c); if (b != 1 && b != 2) c->bad = true; }
        break;
    case 3: pq_byte(c); break;
    case 4: case 5: case 6: pq_var(c); break;
    case 7: pq_take(c, 8, NULL); break;
    case 8: { uint64_t n = pq_var(c); pq_take(c, n, NULL); break; }
    case 9: case 10: {
        uint8_t et; uint64_t n = pq_list(c, &et);
        for (uint64_t i = 0; i < n && !c->bad; i++) pq_skip(c, et, depth+1, true);
        break;
    }
    case 11: {
        uint64_t n = pq_var(c);
        if (n > (uint64_t)(c->end-c->p)/2) { c->bad = true; break; }
        uint8_t h = n ? pq_byte(c) : 0;
        for (uint64_t i = 0; i < n && !c->bad; i++) {
            pq_skip(c, h>>4, depth+1, true); pq_skip(c, h&15, depth+1, true);
        }
        break;
    }
    case 12:
        while (!c->bad) {
            uint8_t h = pq_byte(c);
            if (!h) break;
            if (!(h>>4)) pq_int(c);
            pq_skip(c, h&15, depth+1, false);
        }
        break;
    default: c->bad = true;
    }
    return !c->bad;
}
static bool pq_fields(pq_span s, pq_span* out, size_t n) {
    memset(out, 0, n*sizeof(*out));
    if (s.type != 12) return false;
    pq_cur c = {s.p, s.end, false}; int64_t id = 0;
    while (!c.bad) {
        uint8_t h = pq_byte(&c);
        if (!h) return !c.bad && c.p == c.end;
        id = h>>4 ? id + (h>>4) : pq_int(&c);
        if (id <= 0 || id > INT16_MAX) return false;
        const uint8_t* p = c.p;
        if (!pq_skip(&c, h&15, 0, false)) return false;
        if ((uint64_t)id < n) {
            if (out[id].type) return false;
            out[id] = (pq_span){p, c.p, h&15};
        }
    }
    return false;
}
static bool pq_num(pq_span s, int64_t* v) {
    if (s.type < 4 || s.type > 6) return false;
    pq_cur c = {s.p, s.end, false}; *v = pq_int(&c);
    return !c.bad && c.p == c.end &&
           (s.type != 4 || (*v >= INT16_MIN && *v <= INT16_MAX)) &&
           (s.type != 5 || (*v >= INT32_MIN && *v <= INT32_MAX));
}
static int64_t pq_get(pq_span s, int64_t fallback) {
    int64_t v; return pq_num(s, &v) ? v : fallback;
}
static bool pq_binary(pq_span s, const uint8_t** p, size_t* n) {
    if (s.type != 8) return false;
    pq_cur c = {s.p, s.end, false}; uint64_t len = pq_var(&c);
    if (c.bad || !pq_take(&c, len, p) || c.p != c.end) return false;
    *n = (size_t)len; return true;
}
static bool pq_element(pq_cur* c, uint8_t t, pq_span* s) {
    const uint8_t* start = c->p;
    if (!pq_skip(c, t, 0, true)) return false;
    *s = (pq_span){start, c->p, t}; return true;
}
static ray_t* pq_error(const char* msg) { return ray_error("parquet", "%s", msg); }

bool ray_parquet_snappy(const uint8_t* src, size_t len, uint8_t* dst, size_t size) {
    pq_cur c = {src, src+len, false}; uint64_t expected = pq_var(&c); size_t at = 0;
    if (c.bad || expected != size) return false;
    while (c.p < c.end && at < size) {
        uint8_t tag = pq_byte(&c); size_t n, off;
        if (!(tag&3)) {
            n = tag>>2;
            if (n < 60) n++;
            else {
                unsigned nb = (unsigned)n-59; const uint8_t* p;
                if (!pq_take(&c, nb, &p)) return false;
                uint32_t x = 0;
                for (unsigned i = 0; i < nb; i++) x |= (uint32_t)p[i] << (8*i);
                n = (size_t)x+1;
            }
            const uint8_t* p;
            if (n > size-at || !pq_take(&c, n, &p)) return false;
            memcpy(dst+at, p, n); at += n;
        } else {
            if ((tag&3) == 1) { n = 4+((tag>>2)&7); off = ((size_t)(tag&224)<<3) | pq_byte(&c); }
            else {
                const uint8_t* p; unsigned nb = (tag&3) == 2 ? 2 : 4;
                if (!pq_take(&c, nb, &p)) return false;
                off = nb == 2 ? (size_t)p[0] | (size_t)p[1]<<8 : pq_u32(p);
                n = 1+(tag>>2);
            }
            if (c.bad || !off || off > at || n > size-at) return false;
            /* Copy only initialized, nonoverlapping spans. Small offsets grow
             * geometrically, allowing libc's vector copy even for repeats. */
            size_t copied = 0;
            while (copied < n) {
                size_t step = n-copied < off ? n-copied : off;
                memcpy(dst+at+copied,dst+at+copied-off,step);
                copied += step;
                if (copied >= off) off += off;
            }
            at += n;
        }
    }
    return !c.bad && at == size && c.p == c.end;
}

/* Hybrid RLE/bit-packed streams, resumed across batch boundaries. */
typedef struct {
    pq_cur c;
    uint64_t left, bit;
    const uint8_t* packed;
    uint32_t value;
    unsigned width;
} pq_rle;
static bool pq_rle_next(pq_rle* r, uint32_t* out) {
    if (!r->left) {
        uint64_t h = pq_var(&r->c);
        if (r->c.bad || !h || r->width > 32) return false;
        if (h&1) {
            uint64_t groups = h>>1;
            if (!groups || groups > INT32_MAX/8) return false;
            r->left = groups*8; r->bit = 0;
            if (!pq_take(&r->c, groups*r->width, &r->packed)) return false;
        } else {
            r->left = h>>1; r->packed = NULL; r->value = 0;
            if (r->left > INT32_MAX) return false;
            for (unsigned i = 0; i < (r->width+7)/8; i++) r->value |= (uint32_t)pq_byte(&r->c)<<(8*i);
            if (r->c.bad || (r->width < 32 && r->value >= (UINT32_C(1)<<r->width))) return false;
        }
    }
    if (r->packed) {
        uint64_t v = 0; unsigned shift = (unsigned)(r->bit&7);
        unsigned nb = (shift+r->width+7)/8;
        for (unsigned i = 0; i < nb; i++) v |= (uint64_t)r->packed[r->bit/8+i]<<(8*i);
        *out = (uint32_t)((v>>shift) & (r->width == 32 ? UINT32_MAX : (UINT32_C(1)<<r->width)-1));
        r->bit += r->width;
    } else *out = r->value;
    r->left--; return true;
}
static bool pq_rle_done(const pq_rle* r) {
    return !r->c.bad && r->c.p == r->c.end && (!r->left || (r->packed && r->left < 8));
}

typedef struct {
    int64_t name;
    int physical, optional, converted, type;
    int64_t scale; /* TIMESTAMP multiplier to ns */
    bool native_symbol, strict;
    ray_sym_domain_t* import_domain; /* borrowed from import or scan owner */
} pq_schema;
typedef struct { const uint8_t* p; uint32_t n; } pq_string;
typedef struct {
    const char* strings[8192];
    size_t lengths[8192];
    uint64_t hashes[8192];   /* ray_hash_bytes, all 64 bits */
    int64_t ids[8192], positions[8192];
} pq_symbol_scratch;
typedef struct {
    pq_span metadata;
    pq_cur chunk, values;
    pq_rle defs, ids;
    uint8_t *page, *dict;
    size_t page_cap, dict_cap;
    pq_string* strings;
    int64_t* symbol_ids;
    pq_symbol_scratch* symbols;
    int64_t dict_count, page_left, chunk_left, page_nulls, expected_nulls, pages_skipped;
    uint64_t bool_bit;
    int codec, encoding;
    bool have_dict, optional;
    bool bool_rle;   /* BOOLEAN values RLE-encoded (encoding 3), in `ids` */
} pq_column;
/* A cursor is used by one decoder at a time. Flush each PLAIN batch before
 * advancing pages, so dictionary decoding may reuse the same heap scratch. */
static pq_symbol_scratch* pq_symbols(pq_column* c) {
    if (!c->symbols) c->symbols = ray_alloc_raw(sizeof(*c->symbols));
    return c->symbols;
}
typedef struct { int64_t lo, hi; } pq_interval;
struct ray_parquet {
    uint8_t* map;
    size_t size, data_end;
    pq_schema* schema;
    pq_span* groups;
    int32_t *selected;
    pq_column* cursors;
    ray_sym_domain_t* scan_domain;
    ray_t* text_pattern;
    int64_t text_pos;
    int64_t ncols, ngroups, nselected, noutput, rows, group, group_left, batch_rows;
    int64_t group_batch;  /* batch_rows, capped for this group's STR columns */
    int64_t filter_pos, filter_lo, filter_hi, skipped;
    pq_interval* excluded;
    int64_t nexcluded, exclude_pos, group_rows, bloom_skipped, parallel_batches;
    bool failed, emitted, borrowed, filter_nulls;
};
static bool pq_reserve(uint8_t** buf, size_t* cap, size_t n) {
    if (n > PQ_MAX_PAGE) return false;
    if (!n) n = 1;
    if (n <= *cap) return true;
    size_t nc = *cap ? *cap : 4096;
    while (nc < n) nc *= 2;
    void* p = ray_realloc_raw(*buf, nc);
    if (!p) return false;
    *buf = p; *cap = nc; return true;
}
static bool pq_utf8(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n;) {
        uint32_t c = p[i++]; unsigned more; uint32_t min;
        if (c < 0x80) continue;
        if (c >= 0xc2 && c <= 0xdf) { more = 1; min = 0x80; c &= 31; }
        else if (c >= 0xe0 && c <= 0xef) { more = 2; min = 0x800; c &= 15; }
        else if (c >= 0xf0 && c <= 0xf4) { more = 3; min = 0x10000; c &= 7; }
        else return false;
        if (more > n-i) return false;
        while (more--) { uint8_t b = p[i++]; if ((b&0xc0) != 0x80) return false; c = (c<<6)|(b&63); }
        if (c < min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return false;
    }
    return true;
}
static bool pq_schema_type(pq_schema* s, pq_span* f) {
    int64_t phys, rep;
    if (!pq_num(f[1], &phys) || !pq_num(f[3], &rep) || rep < 0 || rep > 1 || f[5].type) return false;
    s->physical = (int)phys; s->optional = (int)rep;
    s->converted = (int)pq_get(f[6], -1); s->scale = 1;
    switch (phys) {
    case 0: s->type = rep ? RAY_I16 : RAY_BOOL; break;
    case 1: s->type = RAY_I32; break;
    case 2: s->type = RAY_I64; break;
    case 4: s->type = RAY_F32; break;
    case 5: s->type = RAY_F64; break;
    case 6: s->type = RAY_STR; break;
    default: return false;
    }
    int conv = s->converted;
    if (f[10].type) {
        pq_span logical[20];
        if (!pq_fields(f[10], logical, 20)) return false;
        int found = 0;
        for (int i = 1; i < 20; i++) if (logical[i].type) { if (found) return false; found = i; }
        if (found == 1) conv = 0;
        else if (found == 6) conv = 6;
        else if (found == 10) {
            pq_span ints[3]; int64_t bits;
            if (!pq_fields(logical[10], ints, 3) || ints[1].type != 3 || ints[1].end-ints[1].p != 1) return false;
            bits = *ints[1].p;
            if (bits != 8 && bits != 16 && bits != 32 && bits != 64) return false;
            if (ints[2].type != 1 && ints[2].type != 2) return false;
            conv = (ints[2].type == 1 ? 15 : 11) + (bits == 8 ? 0 : bits == 16 ? 1 : bits == 32 ? 2 : 3);
        } else if (found == 8) {
            pq_span ts[3], unit[4];
            if (!pq_fields(logical[8], ts, 3) || (ts[1].type != 1 && ts[1].type != 2) ||
                !pq_fields(ts[2], unit, 4) || phys != 2) return false;
            int units = !!unit[1].type + !!unit[2].type + !!unit[3].type;
            if (units != 1) return false;
            s->scale = unit[1].type ? 1000000 : unit[2].type ? 1000 : 1;
            conv = -2; s->type = RAY_TIMESTAMP;
        } else return false;
    }
    if (conv == 0) { if (phys != 6) return false; }
    else if (conv == 6) { if (phys != 1) return false; s->type = RAY_DATE; }
    else if (conv == 9 || conv == 10) {
        if (phys != 2) return false;
        s->type = RAY_TIMESTAMP; s->scale = conv == 9 ? 1000000 : 1000;
    } else if (conv >= 11 && conv <= 18) {
        if (conv == 14) return false; /* UINT64 has no lossless native counterpart. */
        if (phys != (conv == 18 ? 2 : 1)) return false;
        if (conv == 13) s->type = RAY_I64;
        else if (conv == 11 || conv == 15) s->type = RAY_I16;
        /* I16 annotation stays I32: preserve -32768 without a sentinel collision. */
    } else if (conv != -1 && conv != -2) return false;
    s->converted = conv;
    return true;
}
void ray_parquet_close(ray_parquet_t* r) {
    if (!r) return;
    if (r->cursors) for (int64_t i = 0; i < r->nselected; i++) {
        ray_free_raw(r->cursors[i].page); ray_free_raw(r->cursors[i].dict);
        ray_free_raw(r->cursors[i].strings); ray_free_raw(r->cursors[i].symbol_ids);
        ray_free_raw(r->cursors[i].symbols);
    }
    ray_free_raw(r->cursors); ray_free_raw(r->selected); ray_free_raw(r->excluded);
    if (!r->borrowed) {
        if (r->scan_domain) ray_sym_domain_release(r->scan_domain);
        if (r->text_pattern) ray_release(r->text_pattern);
        ray_free_raw(r->schema); ray_free_raw(r->groups);
        if (r->map) ray_vm_unmap_file(r->map, r->size);
    }
    ray_free_raw(r);
}
ray_t* ray_parquet_open(const char* path, ray_t* columns, int64_t batch_rows, ray_parquet_t** out) {
    *out = NULL;
    if (batch_rows < 1 || batch_rows > PQ_BATCH) return pq_error("batch size must be 1..65536");
    if (columns && (columns->type != RAY_SYM || !columns->len)) return pq_error("columns must be a nonempty symbol vector");
    ray_parquet_t* r = ray_calloc_raw(sizeof(*r));
    if (!r) return ray_error("oom", NULL);
    const char* err = "invalid Parquet footer";
    r->batch_rows = batch_rows; r->group = -1; r->filter_pos = -1;
    r->map = ray_vm_map_file(path, &r->size);
    if (!r->map) { ray_parquet_close(r); return ray_error("io", "parquet: cannot map %s", path); }
    if (r->size < 12 || memcmp(r->map, "PAR1", 4) || memcmp(r->map+r->size-4, "PAR1", 4)) goto fail;
    uint32_t flen = pq_u32(r->map+r->size-8);
    if (flen > r->size-12 || flen > PQ_MAX_FOOTER) goto fail;
    r->data_end = r->size-8-flen;
    pq_span f[10];
    if (!pq_fields((pq_span){r->map+r->data_end,r->map+r->size-8,12}, f, 10) ||
        pq_get(f[1], -1) < 1 || !pq_num(f[3], &r->rows) || r->rows < 0 || f[2].type != 9 || f[4].type != 9 || f[8].type || f[9].type) goto fail;
    pq_cur sc = {f[2].p,f[2].end,false}; uint8_t st;
    uint64_t ns = pq_list(&sc, &st);
    if (sc.bad || st != 12 || ns < 2 || ns > PQ_MAX_COLS+1) goto fail;
    r->ncols = (int64_t)ns-1;
    r->schema = ray_calloc_raw((size_t)r->ncols*sizeof(*r->schema));
    if (!r->schema) { err = "schema allocation failed"; goto fail; }
    pq_span root, sf[11];
    if (!pq_element(&sc, 12, &root) || !pq_fields(root,sf,11) || pq_get(sf[5],-1) != r->ncols || sf[1].type) goto fail;
    err = "unsupported or malformed schema (only flat primitive columns are supported)";
    for (int64_t i = 0; i < r->ncols; i++) {
        pq_span se; const uint8_t* name; size_t len;
        if (!pq_element(&sc,12,&se) || !pq_fields(se,sf,11) || !pq_binary(sf[4],&name,&len) ||
            !len || len > 1024 || memchr(name,0,len) || !pq_schema_type(&r->schema[i],sf)) goto fail;
        r->schema[i].name = ray_sym_intern((const char*)name,len);
        if (r->schema[i].name < 0) goto fail;
        for (int64_t j = 0; j < i; j++) if (r->schema[j].name == r->schema[i].name) goto fail;
    }
    if (sc.p != sc.end) goto fail;
    pq_cur gc = {f[4].p,f[4].end,false}; uint8_t gt;
    uint64_t ng = pq_list(&gc,&gt);
    if (gc.bad || gt != 12 || ng > PQ_MAX_FOOTER/sizeof(pq_span)) goto fail;
    r->ngroups = (int64_t)ng;
    r->groups = ray_calloc_raw((size_t)(ng ? ng : 1)*sizeof(*r->groups));
    if (!r->groups) goto fail;
    int64_t total = 0;
    err = "invalid row group metadata";
    for (uint64_t i = 0; i < ng; i++) {
        pq_span gf[8]; int64_t nr;
        if (!pq_element(&gc,12,&r->groups[i]) || !pq_fields(r->groups[i],gf,8) ||
            !pq_num(gf[3],&nr) || nr < 0 || nr > r->rows-total || gf[1].type != 9) goto fail;
        pq_cur chunks = {gf[1].p,gf[1].end,false}; uint8_t chunk_type;
        if (pq_list(&chunks,&chunk_type) != (uint64_t)r->ncols || chunks.bad || chunk_type != 12) goto fail;
        total += nr;
    }
    if (gc.p != gc.end || total != r->rows) goto fail;
    r->nselected = columns ? columns->len : r->ncols;
    r->noutput = r->nselected;
    if (r->nselected > r->ncols) goto fail;
    r->selected = ray_alloc_raw((size_t)r->ncols*sizeof(*r->selected));
    r->cursors = ray_calloc_raw((size_t)r->ncols*sizeof(*r->cursors));
    if (!r->selected || !r->cursors) goto fail;
    err = "unknown or duplicate projection column";
    for (int64_t i = 0; i < r->nselected; i++) {
        ray_t* cs = columns ? ray_sym_vec_cell(columns,i) : NULL;
        int64_t name = columns ? (cs ? ray_sym_find(ray_str_ptr(cs),ray_str_len(cs)) : -1) : r->schema[i].name;
        int64_t j = 0;
        while (j < r->ncols && r->schema[j].name != name) j++;
        if (j == r->ncols) goto fail;
        for (int64_t k = 0; k < i; k++) if (r->selected[k] == j) goto fail;
        r->selected[i] = (int32_t)j;
    }
    *out = r; return NULL;
fail:
    ray_parquet_close(r); return pq_error(err);
}

ray_t* ray_parquet_range(ray_parquet_t* r, int64_t column, int64_t lo, int64_t hi) {
    if (!r || r->group != -1 || r->emitted || r->filter_pos >= 0 || lo > hi)
        return pq_error("range must be configured once before scanning, with lo <= hi");
    int64_t c = 0;
    while (c < r->ncols && r->schema[c].name != column) c++;
    if (c == r->ncols) return pq_error("unknown range column");
    pq_schema* s = &r->schema[c];
    if ((s->type != RAY_I16 && s->type != RAY_I32 && s->type != RAY_I64) ||
        (s->converted != -1 && (s->converted < 15 || s->converted > 18)))
        return pq_error("range requires a signed integer column");
    int64_t pos = 0;
    while (pos < r->nselected && r->selected[pos] != c) pos++;
    if (pos == r->nselected) r->selected[r->nselected++] = (int32_t)c;
    r->filter_pos = pos; r->filter_lo = lo; r->filter_hi = hi;
    return NULL;
}
int64_t ray_parquet_groups_skipped(const ray_parquet_t* r) { return r ? r->skipped : 0; }

static bool pq_disjoint(ray_parquet_t* r, pq_span metadata) {
    pq_span mf[17], stats[10];
    if (!pq_fields(metadata,mf,17) || !pq_fields(mf[12],stats,10)) return false;
    const uint8_t *minp, *maxp; size_t minn, maxn;
    /* Legacy bounds have signed ordering. Modern bounds for the eligible
     * signed integer types have the same ordering. Missing bounds => scan. */
    pq_span low = stats[6].type ? stats[6] : stats[2];
    pq_span high = stats[5].type ? stats[5] : stats[1];
    if (!pq_binary(low,&minp,&minn) || !pq_binary(high,&maxp,&maxn)) return false;
    int physical = r->schema[r->selected[r->filter_pos]].physical;
    size_t width = physical == 1 ? 4 : 8;
    if (minn != width || maxn != width) return false;
    int64_t lo = width == 4 ? (int32_t)pq_u32(minp) : (int64_t)pq_u64(minp);
    int64_t hi = width == 4 ? (int32_t)pq_u32(maxp) : (int64_t)pq_u64(maxp);
    return lo <= hi && (hi < r->filter_lo || lo > r->filter_hi);
}

/* Optional metadata is advisory: unsupported or malformed indexes never
 * exclude rows. Only the signed integer ordering admitted by range() is used. */
static bool pq_bounds_disjoint(ray_parquet_t* r, pq_span low, pq_span high) {
    const uint8_t *a, *b; size_t an, bn;
    size_t width = r->schema[r->selected[r->filter_pos]].physical == 1 ? 4 : 8;
    if (!pq_binary(low,&a,&an) || !pq_binary(high,&b,&bn) || an != width || bn != width) return false;
    int64_t lo = width == 4 ? (int32_t)pq_u32(a) : (int64_t)pq_u64(a);
    int64_t hi = width == 4 ? (int32_t)pq_u32(b) : (int64_t)pq_u64(b);
    return lo <= hi && (hi < r->filter_lo || lo > r->filter_hi);
}
static bool pq_index_span(ray_parquet_t* r, pq_span off, pq_span len, pq_span* out) {
    int64_t o, n;
    if (!pq_num(off,&o) || !pq_num(len,&n) || o < 4 || n <= 0 || n > PQ_MAX_FOOTER ||
        (uint64_t)o > r->data_end || (uint64_t)n > r->data_end-(size_t)o) return false;
    *out = (pq_span){r->map+o,r->map+o+n,12}; return true;
}
/* XXH64, seed zero, specialized to the 4/8 byte PLAIN integer encodings.
 * Derived from the public algorithm specification; no library implementation. */
static uint64_t pq_rot(uint64_t x, unsigned n) { return (x<<n) | (x>>(64-n)); }
static uint64_t pq_integer_hash(uint64_t x, unsigned width) {
    const uint64_t p1 = UINT64_C(11400714785074694791), p2 = UINT64_C(14029467366897019727);
    const uint64_t p3 = UINT64_C(1609587929392839161), p4 = UINT64_C(9650029242287828579);
    uint64_t h = UINT64_C(2870177450012600261)+width;
    if (width == 8) { h ^= pq_rot(x*p2,31)*p1; h = pq_rot(h,27)*p1+p4; }
    else { h ^= (uint64_t)(uint32_t)x*p1; h = pq_rot(h,23)*p2+p3; }
    h ^= h>>33; h *= p2; h ^= h>>29; h *= p3; return h ^ (h>>32);
}
static bool pq_bloom_absent(ray_parquet_t* r, pq_span metadata) {
    if (r->filter_lo != r->filter_hi) return false;
    pq_span m[17], f[5]; int64_t offset;
    if (!pq_fields(metadata,m,17) || !pq_num(m[14],&offset) || offset < 4 ||
        (uint64_t)offset >= r->data_end) return false;
    size_t avail = r->data_end-(size_t)offset;
    if (avail > PQ_MAX_PAGE+1024) avail = PQ_MAX_PAGE+1024;
    int64_t length;
    if (m[15].type) {
        if (!pq_num(m[15],&length) || length <= 0 || (uint64_t)length > avail) return false;
        avail = (size_t)length;
    }
    pq_cur c = {r->map+offset,r->map+offset+avail,false};
    const uint8_t* start = c.p;
    if (!pq_skip(&c,12,0,false) || !pq_fields((pq_span){start,c.p,12},f,5)) return false;
    int64_t n = pq_get(f[1],-1);
    if (n < 32 || n > PQ_MAX_PAGE || n%32 || (uint64_t)n > (uint64_t)(c.end-c.p)) return false;
    if (m[15].type && n != c.end-c.p) return false;
    /* Each supported union is exactly {1: empty struct}. Unknown variants
     * (including future compression/hash strategies) require a normal scan. */
    for (int i = 2; i <= 4; i++) {
        static const uint8_t supported[] = {0x1c,0,0};
        if (f[i].type != 12 || f[i].end-f[i].p != 3 || memcmp(f[i].p,supported,3)) return false;
    }
    unsigned width = r->schema[r->selected[r->filter_pos]].physical == 1 ? 4 : 8;
    if (width == 4 && (r->filter_lo < INT32_MIN || r->filter_lo > INT32_MAX)) return true;
    uint64_t h = pq_integer_hash((uint64_t)r->filter_lo,width);
    const uint8_t* block = c.p+(((h>>32)*(uint64_t)(n/32))>>32)*32;
    static const uint32_t salt[8] = {0x47b6137b,0x44974d91,0x8824ad5b,0xa2b7289d,0x705495c7,0x2df1424b,0x9efc4947,0x5c6bfb31};
    for (int i = 0; i < 8; i++)
        if (!(pq_u32(block+4*i) & (UINT32_C(1)<<(((uint32_t)h*salt[i])>>27)))) return true;
    return false;
}
static void pq_page_intervals(ray_parquet_t* r, pq_span chunk) {
    pq_span cf[10], os, cs, of[3], f[9];
    if (!r->group_rows || !pq_fields(chunk,cf,10) ||
        !pq_index_span(r,cf[4],cf[5],&os) || !pq_index_span(r,cf[6],cf[7],&cs) ||
        !pq_fields(os,of,3) || !pq_fields(cs,f,9) || of[1].type != 9 ||
        f[1].type != 9 || f[2].type != 9 || f[3].type != 9 || pq_get(f[4],-1) < 0 || pq_get(f[4],3) > 2) return;
    pq_cur loc = {of[1].p,of[1].end,false}, nulls = {f[1].p,f[1].end,false};
    pq_cur min = {f[2].p,f[2].end,false}, max = {f[3].p,f[3].end,false};
    uint8_t lt, nt, at, bt;
    uint64_t n = pq_list(&loc,&lt), nn = pq_list(&nulls,&nt), na = pq_list(&min,&at), nb = pq_list(&max,&bt);
    if (loc.bad || nulls.bad || min.bad || max.bad || !n || n > PQ_MAX_FOOTER/sizeof(pq_interval) ||
        n != nn || n != na || n != nb || lt != 12 || nt != 1 || at != 8 || bt != 8) return;
    pq_interval* ranges = ray_alloc_raw((size_t)n*sizeof(*ranges));
    if (!ranges) return; /* Optional optimization. */
    int64_t expected_row = 0, nranges = 0;
    const pq_column* col = &r->cursors[r->filter_pos];
    const uint8_t* expected_page = NULL;
    for (uint64_t i = 0; i < n; i++) {
        pq_span ls, lf[4], low, high, hf[10], dh[9];
        if (!pq_element(&loc,12,&ls) || !pq_fields(ls,lf,4) ||
            !pq_element(&min,8,&low) || !pq_element(&max,8,&high)) goto invalid;
        uint8_t isnull = pq_byte(&nulls);
        int64_t off = pq_get(lf[1],-1), bytes = pq_get(lf[2],-1), row = pq_get(lf[3],-1);
        if ((isnull != 1 && isnull != 2) || row != expected_row || off < 4 || bytes <= 0 ||
            (uint64_t)off > r->data_end || (uint64_t)bytes > r->data_end-(size_t)off) goto invalid;
        const uint8_t* p = r->map+off;
        if (p < col->chunk.p || p+bytes > col->chunk.end || (expected_page && p != expected_page)) goto invalid;
        if (!i) {
            pq_span mf[17];
            if (!pq_fields(col->metadata,mf,17) || off != pq_get(mf[9],-1)) goto invalid;
        }
        pq_cur page = {p,p+bytes,false};
        if (!pq_skip(&page,12,0,false) || !pq_fields((pq_span){p,page.p,12},hf,10)) goto invalid;
        int64_t kind = pq_get(hf[1],-1), count;
        if ((kind != 0 && kind != 3) || pq_get(hf[3],-1) != page.end-page.p ||
            !pq_fields(hf[kind == 0 ? 5 : 8],dh,9) || !pq_num(dh[1],&count) || count <= 0 ||
            count > r->group_rows-row) goto invalid;
        expected_row += count; expected_page = page.end;
        if (isnull == 1 || pq_bounds_disjoint(r,low,high)) {
            if (nranges && ranges[nranges-1].hi == row) ranges[nranges-1].hi += count;
            else ranges[nranges++] = (pq_interval){row,row+count};
        }
    }
    if (expected_row != r->group_rows || expected_page != col->chunk.end || loc.p != loc.end ||
        nulls.bad || nulls.p != nulls.end || min.p != min.end || max.p != max.end) goto invalid;
    r->excluded = ranges; r->nexcluded = nranges; return;
invalid:
    ray_free_raw(ranges);
}

static bool pq_group_columns(ray_parquet_t* r, int64_t g, pq_span* cols, int64_t* rows, pq_span* filter_chunk) {
    pq_span gf[8];
    if (!pq_fields(r->groups[g],gf,8) || !pq_num(gf[3],rows) || gf[1].type != 9) return false;
    pq_cur c = {gf[1].p,gf[1].end,false}; uint8_t type;
    if (pq_list(&c,&type) != (uint64_t)r->ncols || c.bad || type != 12) return false;
    for (int64_t i = 0; i < r->ncols; i++) {
        pq_span ch, cf[10], mf[17];
        if (!pq_element(&c,12,&ch) || !pq_fields(ch,cf,10) || cf[1].type || cf[8].type || cf[9].type ||
            !pq_fields(cf[3],mf,17) || pq_get(mf[1],-1) != r->schema[i].physical ||
            pq_get(mf[5],-1) != *rows || mf[3].type != 9) return false;
        pq_cur path = {mf[3].p,mf[3].end,false}; uint8_t pt; pq_span ps; const uint8_t* p; size_t n;
        if (pq_list(&path,&pt) != 1 || pt != 8 || !pq_element(&path,pt,&ps) ||
            !pq_binary(ps,&p,&n) || path.p != path.end) return false;
        const char* name = ray_str_ptr(ray_sym_str(r->schema[i].name));
        if (!name || strlen(name) != n || memcmp(name,p,n)) return false;
        cols[i] = cf[3];
        if (filter_chunk && i == r->selected[r->filter_pos]) *filter_chunk = ch;
    }
    return c.p == c.end;
}
/* Longest entry in a BYTE_ARRAY column chunk's dictionary page, 0 when it
 * has none (or it cannot be read here; the decoder reports that later).
 * Every dictionary-encoded row copies one entry into the batch pool, and
 * the chunk's uncompressed size (dictionary + indices) says nothing about
 * how many times a long entry repeats. */
static int64_t pq_dict_max_len(ray_parquet_t* r, pq_span meta) {
    pq_span mf[17];
    if (!pq_fields(meta,mf,17)) return 0;
    int64_t dict = pq_get(mf[11],-1), codec = pq_get(mf[4],-1);
    if (dict < 4 || (uint64_t)dict >= r->data_end) return 0;
    pq_cur c = {r->map+dict,r->map+r->data_end,false};
    const uint8_t* start = c.p;
    if (!pq_skip(&c,12,0,false)) return 0;
    pq_span f[10];
    if (!pq_fields((pq_span){start,c.p,12},f,10) || pq_get(f[1],-1) != 2) return 0;
    int64_t raw = pq_get(f[2],-1), size = pq_get(f[3],-1);
    if (raw < 0 || raw > PQ_MAX_PAGE || size < 0 || size > PQ_MAX_PAGE ||
        (uint64_t)size > (uint64_t)(c.end-c.p)) return 0;
    const uint8_t* p = c.p; uint8_t* buf = NULL;
    if (codec == 1) {
        buf = ray_alloc_raw((size_t)(raw ? raw : 1));
        if (!buf || !ray_parquet_snappy(c.p,(size_t)size,buf,(size_t)raw)) { ray_free_raw(buf); return 0; }
        p = buf;
    } else if (raw != size) return 0;
    int64_t max = 0;
    for (const uint8_t* q = p, *end = p+raw; end-q >= 4; ) {
        uint32_t len = pq_u32(q); q += 4;
        if (len > (uint64_t)(end-q)) break;
        if ((int64_t)len > max) max = len;
        q += len;
    }
    ray_free_raw(buf);
    return max;
}

/* Rows per batch for row group g: batch_rows, lowered so each selected STR
 * column's pool stays near PQ_POOL_TARGET.  Bytes per row are bounded by
 * the larger of the chunk's uncompressed bytes per value (PLAIN pages hold
 * every value's bytes; a 4 MB file of 11 KB documents decodes 86 MB of
 * strings, and one fixed 65,536-row batch would hold all of it) and the
 * longest dictionary entry (dictionary pages; each row may repeat it).
 * PQ_POOL_MAX remains the hard limit.  Deterministic from the file, so the
 * partitioned writer can count partitions with it before decoding.  `cols`
 * is the group's column chunk metadata (pq_group_columns). */
static int64_t pq_cols_batch(ray_parquet_t* r, const pq_span* cols) {
    int64_t batch = r->batch_rows;
    for (int64_t i = 0; i < r->nselected; i++) {
        const pq_schema* sc = &r->schema[r->selected[i]];
        if (sc->physical != 6 || sc->type != RAY_STR || sc->import_domain) continue;
        pq_span mf[17];
        if (!pq_fields(cols[r->selected[i]],mf,17)) continue;
        int64_t values = pq_get(mf[5],-1), bytes = pq_get(mf[6],-1);
        if (values <= 0 || bytes <= 0) continue;
        int64_t per_row = bytes/values + 1;
        int64_t longest = pq_dict_max_len(r,cols[r->selected[i]]) + 1;
        if (longest > per_row) per_row = longest;
        int64_t cap = (int64_t)PQ_POOL_TARGET/per_row;
        if (cap < 1) cap = 1;
        if (cap < batch) batch = cap;
    }
    return batch;
}
static int64_t pq_group_batch(ray_parquet_t* r, int64_t g) {
    int64_t batch = r->batch_rows, rows;
    pq_span* cols = ray_alloc_raw((size_t)r->ncols*sizeof(*cols));
    if (!cols) return batch;
    if (pq_group_columns(r,g,cols,&rows,NULL)) batch = pq_cols_batch(r,cols);
    ray_free_raw(cols);
    return batch;
}
static const char* pq_start_group(ray_parquet_t* r) {
    pq_span* cols = ray_alloc_raw((size_t)r->ncols*sizeof(*cols));
    if (!cols) return "row group allocation failed";
    pq_span filter_chunk = {0};
    ray_free_raw(r->excluded); r->excluded = NULL; r->nexcluded = r->exclude_pos = 0;
    if (!pq_group_columns(r,r->group,cols,&r->group_left,r->filter_pos >= 0 ? &filter_chunk : NULL)) { ray_free_raw(cols); return "invalid column chunk metadata"; }
    /* A zero-row group has nothing to decode.  Writers still emit its
     * chunks, with a dictionary page and data_page_offset 0, which the
     * chunk checks below would reject. */
    if (r->group_left == 0) { ray_free_raw(cols); return NULL; }
    r->group_batch = pq_cols_batch(r,cols);   /* the metadata just read, not read again */
    /* Bounds and Bloom filters describe non-null values. A WHERE accepting
     * nulls must decode these groups/pages, including native null sentinels
     * in required columns. Explicit read ranges still exclude nulls. */
    if (r->filter_pos >= 0 && !r->filter_nulls && pq_disjoint(r,cols[r->selected[r->filter_pos]])) {
        r->group_left = 0; r->skipped++; ray_free_raw(cols); return NULL;
    }
    if (r->filter_pos >= 0 && !r->filter_nulls && pq_bloom_absent(r,cols[r->selected[r->filter_pos]])) {
        r->group_left = 0; r->skipped++; r->bloom_skipped++; ray_free_raw(cols); return NULL;
    }
    r->group_rows = r->group_left;
    const char* err = NULL;
    for (int64_t i = 0; i < r->nselected; i++) {
        pq_column* c = &r->cursors[i]; pq_span mf[17];
        c->metadata = cols[r->selected[i]];
        if (!pq_fields(c->metadata,mf,17)) { err = "invalid column metadata"; break; }
        int64_t offset = pq_get(mf[9],-1), dict = pq_get(mf[11],-1), bytes = pq_get(mf[7],-1);
        int64_t codec = pq_get(mf[4],-1);
        if (codec != 0 && codec != 1) { err = "unsupported compression codec (supported: uncompressed, Snappy)"; break; }
        if (dict >= 0) { if (dict > offset) { err = "dictionary follows data"; break; } offset = dict; }
        if (offset < 4 || bytes < 0 || (uint64_t)offset > r->data_end || (uint64_t)bytes > r->data_end-(size_t)offset) {
            err = "column chunk outside file data"; break;
        }
        c->chunk = (pq_cur){r->map+offset,r->map+offset+bytes,false};
        c->codec = (int)codec; c->page_left = 0; c->chunk_left = r->group_left;
        c->have_dict = false; c->dict_count = 0; c->optional = r->schema[r->selected[i]].optional;
        ray_free_raw(c->strings); c->strings = NULL;
        ray_free_raw(c->symbol_ids); c->symbol_ids = NULL;
    }
    if (!err && r->filter_pos >= 0 && !r->filter_nulls) pq_page_intervals(r,filter_chunk);
    ray_free_raw(cols); return err;
}
static bool pq_unpack(pq_column* c, const uint8_t* p, size_t n, size_t decoded, bool compressed, const uint8_t** out) {
    if (decoded > PQ_MAX_PAGE) return false;
    if (!compressed || !c->codec) { if (n != decoded) return false; *out = p; return true; }
    if (!pq_reserve(&c->page,&c->page_cap,decoded)) return false;
    if (!ray_parquet_snappy(p,n,c->page,decoded)) return false;
    *out = c->page; return true;
}
/* A dictionary page of `size` bytes in the file, `n` decoded: decompressed
 * straight into c->dict (not through c->page and a copy), or copied there
 * when stored. */
static bool pq_dictionary(pq_column* c, pq_schema* s, const uint8_t* payload, size_t size, size_t n, int64_t count) {
    if (c->have_dict || count < 0 || (uint64_t)count > PQ_MAX_PAGE/sizeof(pq_string)) return false;
    if (!pq_reserve(&c->dict,&c->dict_cap,n)) return false;
    if (c->codec) { if (!ray_parquet_snappy(payload,size,c->dict,n)) return false; }
    else if (size != n) return false;
    else if (n) memcpy(c->dict,payload,n);
    c->dict_count = count; c->have_dict = true;
    if (s->physical == 6) {
        c->strings = ray_calloc_raw((size_t)(count ? count : 1)*sizeof(*c->strings));
        if (!c->strings) return false;
        pq_cur p = {c->dict,c->dict+n,false};
        for (int64_t i = 0; i < count; i++) {
            const uint8_t* len; const uint8_t* str;
            if (!pq_take(&p,4,&len)) return false;
            uint32_t size = pq_u32(len);
            if (!pq_take(&p,size,&str)) return false;
            if (s->strict && s->converted == 0 && !pq_utf8(str,size)) return false;
            c->strings[i] = (pq_string){str,size};
        }
        if (p.p != p.end) return false;
        if (s->import_domain) {
            c->symbol_ids = ray_alloc_raw((size_t)(count ? count : 1)*sizeof(*c->symbol_ids));
            if (!c->symbol_ids) return false;
            pq_symbol_scratch* scratch = pq_symbols(c);
            if (!scratch) return false;
            const char** strings = scratch->strings; size_t* lengths = scratch->lengths;
            uint64_t* hashes = scratch->hashes;
            for (int64_t off = 0; off < count; off += 8192) {
                int64_t n = count-off < 8192 ? count-off : 8192;
                for (int64_t i = 0; i < n; i++) {
                    strings[i] = (const char*)c->strings[off+i].p; lengths[i] = c->strings[off+i].n;
                    hashes[i] = ray_hash_bytes(strings[i],lengths[i]);
                }
                if (!ray_sym_domain_intern_batch64(s->import_domain,n,strings,lengths,hashes,c->symbol_ids+off)) return false;
            }
        }
        return true;
    }
    size_t width = s->physical == 1 || s->physical == 4 ? 4 : 8;
    return s->physical != 0 && (uint64_t)count*width == n;
}
static const char* pq_page(pq_column* c, pq_schema* s, int64_t* skip) {
    while (c->chunk.p < c->chunk.end) {
        const uint8_t* start = c->chunk.p;
        if (!pq_skip(&c->chunk,12,0,false)) return "invalid page header";
        pq_span f[10];
        if (!pq_fields((pq_span){start,c->chunk.p,12},f,10)) return "invalid page fields";
        int64_t kind = pq_get(f[1],-1), raw = pq_get(f[2],-1), size = pq_get(f[3],-1);
        if (raw < 0 || raw > PQ_MAX_PAGE || size < 0 || size > PQ_MAX_PAGE) return "invalid or oversized page (limit 64 MiB)";
        const uint8_t* payload;
        if (!pq_take(&c->chunk,(uint64_t)size,&payload)) return "truncated page";
        if (skip && (kind == 0 || kind == 3)) {
            pq_span h[9]; int64_t count;
            if (!pq_fields(f[kind == 0 ? 5 : 8],h,9) || !pq_num(h[1],&count) ||
                count <= 0 || count > c->chunk_left) return "invalid skipped page row count";
            if (count <= *skip) {
                *skip -= count; c->chunk_left -= count; c->pages_skipped++;
                if (!c->chunk_left && c->chunk.p != c->chunk.end) return "excess column pages";
                if (!*skip) return NULL;
                continue;
            }
        }
        if (f[4].type) {
            int64_t expected;
            if (!pq_num(f[4],&expected)) return "invalid page checksum";
            /* Ordinary CRC-32 over the compressed page body. */
            if (ray_crc32(0,payload,(size_t)size) != (uint32_t)expected) return "page checksum mismatch";
        }
        if (kind == 1) continue; /* legacy index page */
        const uint8_t* data = NULL; pq_span h[9]; int64_t count, encoding;
        c->bool_bit = 0; c->bool_rle = false; c->page_nulls = 0; c->expected_nulls = -1;
        if (kind == 2) {
            if (!pq_fields(f[7],h,9) || !pq_num(h[1],&count) ||
                (pq_get(h[2],-1) != 0 && pq_get(h[2],-1) != 2) ||
                !pq_dictionary(c,s,payload,(size_t)size,(size_t)raw,count)) return "invalid dictionary page";
            continue;
        }
        if (kind == 0) {
            if (!pq_fields(f[5],h,9) || !pq_num(h[1],&count) || !pq_num(h[2],&encoding) ||
                (c->optional && pq_get(h[3],-1) != 3) ||
                !pq_unpack(c,payload,(size_t)size,(size_t)raw,true,&data)) return "invalid v1 data page";
            c->values = (pq_cur){data,data+raw,false};
            if (c->optional) {
                const uint8_t* p; const uint8_t* levels;
                if (!pq_take(&c->values,4,&p) || !pq_take(&c->values,pq_u32(p),&levels)) return "truncated definition levels";
                c->defs = (pq_rle){.c={levels,c->values.p,false},.width=1};
            }
        } else if (kind == 3) {
            int64_t dl, rl, nulls, nr;
            if (!pq_fields(f[8],h,9) || !pq_num(h[1],&count) || !pq_num(h[2],&nulls) ||
                !pq_num(h[3],&nr) || nr != count || nulls < 0 || nulls > count ||
                (!c->optional && nulls) || !pq_num(h[4],&encoding) || !pq_num(h[5],&dl) ||
                !pq_num(h[6],&rl) || dl < 0 || rl != 0 || dl > size || dl > raw ||
                (!c->optional && dl) || (h[7].type && h[7].type != 1 && h[7].type != 2)) return "invalid v2 data page";
            c->defs = (pq_rle){.c={payload,payload+dl,false},.width=1};
            if (!pq_unpack(c,payload+dl,(size_t)(size-dl),(size_t)(raw-dl),h[7].type != 2,&data)) return "invalid v2 compressed values";
            c->values = (pq_cur){data,data+raw-dl,false};
            c->expected_nulls = nulls;
        } else return "unsupported page type";
        if (count <= 0 || count > c->chunk_left) return "invalid page row count";
        if (encoding == 3 && s->physical == 0) {
            /* RLE booleans (the v2 default of common writers): a 4-byte
             * little-endian length, then a bit-width-1 hybrid stream. */
            const uint8_t* lp; const uint8_t* body;
            if (!pq_take(&c->values,4,&lp) || !pq_take(&c->values,pq_u32(lp),&body) ||
                c->values.p != c->values.end) return "invalid RLE boolean data";
            c->ids = (pq_rle){.c={body,c->values.end,false},.width=1};
            c->bool_rle = true; c->encoding = 0; c->page_left = count;
            return NULL;
        }
        if (encoding != 0 && encoding != 2 && encoding != 8) return "unsupported value encoding (supported: plain, dictionary, RLE booleans)";
        c->encoding = (int)encoding; c->page_left = count;
        if (encoding) {
            if (!c->have_dict || s->physical == 0) return "missing or invalid dictionary";
            unsigned width = pq_byte(&c->values);
            if (c->values.bad || width > 32) return "invalid dictionary bit width";
            c->ids = (pq_rle){.c=c->values,.width=width};
        }
        return NULL;
    }
    return "missing data page";
}

#define PQ_PUT_FAIL "value conversion or allocation failed"
/* Convert one value into row `row`; NULL, or the reason it failed. */
static const char* pq_put_value(ray_t** vp, int64_t row, pq_schema* s, const uint8_t* p, uint32_t len) {
    ray_t* v = *vp;
    switch (s->physical) {
    case 0:
        if (v->type == RAY_BOOL) ((uint8_t*)ray_data(v))[row] = *p;
        else ((int16_t*)ray_data(v))[row] = *p;
        break;
    case 1: case 2: {
        int64_t x = s->physical == 1 ? (int32_t)pq_u32(p) : (int64_t)pq_u64(p);
        if (s->physical == 1 && s->converted >= 11 && s->converted <= 13) x = pq_u32(p);
        if ((s->converted == 11 && x > UINT8_MAX) ||
            (s->converted == 12 && x > UINT16_MAX) ||
            (s->converted == 15 && (x < INT8_MIN || x > INT8_MAX)) ||
            (s->converted == 16 && (x < INT16_MIN || x > INT16_MAX))) return PQ_PUT_FAIL;
        if (s->type == RAY_DATE && __builtin_sub_overflow(x,PQ_EPOCH_DAYS,&x)) return PQ_PUT_FAIL;
        int64_t raw = x;
        if (s->type == RAY_TIMESTAMP &&
            (__builtin_sub_overflow(x,PQ_EPOCH_NS/s->scale,&x) || __builtin_mul_overflow(x,s->scale,&x))) {
            /* Outside the native nanosecond range, e.g. a 9999-12-31 "end
             * of time" sentinel at ms precision.  Clamp to the nearest
             * representable instant so comparisons keep their meaning (a
             * null would sort below every timestamp and read as expired);
             * strict mode rejects the value instead. */
            if (s->strict) return "timestamp outside the native nanosecond range (strict mode)";
            x = raw >= PQ_EPOCH_NS/s->scale ? INT64_MAX : INT64_MIN+1;
        }
        if (s->type == RAY_I16) {
            if (x < INT16_MIN || x > INT16_MAX) return PQ_PUT_FAIL;
            ((int16_t*)ray_data(v))[row] = (int16_t)x;
        } else if (s->type == RAY_I64 || s->type == RAY_TIMESTAMP) ((int64_t*)ray_data(v))[row] = x;
        else {
            if (x < INT32_MIN || x > INT32_MAX) return PQ_PUT_FAIL;
            ((int32_t*)ray_data(v))[row] = (int32_t)x;
        }
        break;
    }
    case 4: { uint32_t u = pq_u32(p); memcpy((float*)ray_data(v)+row,&u,4); break; }
    case 5: { uint64_t u = pq_u64(p); memcpy((double*)ray_data(v)+row,&u,8); break; }
    case 6: {
        if (s->strict && s->converted == 0 && !pq_utf8(p,len)) return PQ_PUT_FAIL;
        ray_t* next = ray_str_vec_set(v,row,(const char*)p,len);
        if (!next || RAY_IS_ERR(next)) { if (next) ray_release(next); return PQ_PUT_FAIL; }
        *vp = v = next; break;
    }
    default: return PQ_PUT_FAIL;
    }
    uint8_t attrs = v->attrs;
    v->attrs |= RAY_ATTR_HAS_NULLS;
    if (!ray_vec_is_null(v,row)) v->attrs = attrs;
    else if (s->strict) return PQ_PUT_FAIL;
    return NULL;
}
/* PLAIN signed integers can go straight into native blocks. SSE2 handles
 * copy and sentinel detection together; scalar code also handles big endian. */
static void pq_plain_int(ray_t* v, int64_t row, const uint8_t* p, int64_t n) {
    int64_t i = 0;
    size_t width = v->type == RAY_I32 ? 4 : 8;
    uint8_t* dst = (uint8_t*)ray_data(v)+(size_t)row*width;
#if defined(__SSE2__)
    __m128i sentinel = width == 4 ? _mm_set1_epi32(INT32_MIN) : _mm_set_epi32(INT32_MIN,0,INT32_MIN,0);
    for (; i+16/(int64_t)width <= n; i += 16/(int64_t)width) {
        __m128i x = _mm_loadu_si128((const __m128i*)(p+(size_t)i*width));
        _mm_storeu_si128((__m128i*)(dst+(size_t)i*width),x);
        int mask = _mm_movemask_epi8(_mm_cmpeq_epi32(x,sentinel));
        if ((width == 4 && mask) || (width == 8 && ((mask&255) == 255 || (mask>>8) == 255)))
            v->attrs |= RAY_ATTR_HAS_NULLS;
    }
#endif
    for (; i < n; i++) {
        if (width == 4) {
            int32_t x = (int32_t)pq_u32(p+(size_t)i*4); memcpy(dst+(size_t)i*4,&x,4);
            if (x == INT32_MIN) v->attrs |= RAY_ATTR_HAS_NULLS;
        } else {
            int64_t x = (int64_t)pq_u64(p+(size_t)i*8); memcpy(dst+(size_t)i*8,&x,8);
            if (x == INT64_MIN) v->attrs |= RAY_ATTR_HAS_NULLS;
        }
    }
}
/* Native dictionary ids go directly to file-domain symbol positions. PLAIN
 * pages intern bounded batches before their decompression buffer is reused. */
static const char* pq_decode_symbols(pq_column* c, pq_schema* s, ray_t** out, int64_t rows) {
    pq_symbol_scratch* scratch = pq_symbols(c);
    if (!scratch) return "symbol scratch allocation failed";
    ray_t* v = ray_sym_vec_new(RAY_SYM_W32,rows);
    if (!v || RAY_IS_ERR(v)) { if (v) ray_release(v); return "symbol vector allocation failed"; }
    v->sym_domain = s->import_domain; ray_sym_domain_retain(v->sym_domain);
    v->len = rows; *out = v; uint32_t* dst = ray_data(v);
    const char** strings = scratch->strings; size_t* lengths = scratch->lengths;
    uint64_t* hashes = scratch->hashes;
    int64_t* ids = scratch->ids; int64_t* positions = scratch->positions;
    for (int64_t at = 0; at < rows;) {
        if (ray_interrupted()) return "scan interrupted";
        if (!c->page_left) { const char* err = pq_page(c,s,NULL); if (err) return err; }
        int64_t n = rows-at < c->page_left ? rows-at : c->page_left, count = 0;
        if (n > 8192) n = 8192;
        for (int64_t i = 0; i < n; i++) {
            if (!c->optional && c->encoding && c->ids.left && !c->ids.packed) {
                uint32_t id = c->ids.value;
                if (id >= c->dict_count || !c->symbol_ids) return "invalid symbol dictionary index";
                int64_t pos = c->symbol_ids[id];
                if (pos < 0 || (uint64_t)pos >= UINT32_MAX) return "symbol domain exceeds W32";
                if (!pos && s->strict) return "nonnull empty text collides with native null";
                int64_t count = n-i < (int64_t)c->ids.left ? n-i : (int64_t)c->ids.left;
                for (int64_t j = 0; j < count; j++) dst[at+i+j] = (uint32_t)pos;
                if (!pos) v->attrs |= RAY_ATTR_HAS_NULLS;
                c->ids.left -= (uint64_t)count; i += count-1; continue;
            }
            uint32_t present = 1;
            if (c->optional && (!pq_rle_next(&c->defs,&present) || present > 1)) return "invalid definition levels";
            if (!present) { dst[at+i] = 0; v->attrs |= RAY_ATTR_HAS_NULLS; c->page_nulls++; continue; }
            if (c->encoding) {
                uint32_t id;
                if (!pq_rle_next(&c->ids,&id) || id >= c->dict_count || !c->symbol_ids) return "invalid symbol dictionary index";
                int64_t pos = c->symbol_ids[id];
                if (pos < 0 || (uint64_t)pos >= UINT32_MAX) return "symbol domain exceeds W32";
                if (!pos && s->strict) return "nonnull empty text collides with native null";
                dst[at+i] = (uint32_t)pos;
                if (!pos) v->attrs |= RAY_ATTR_HAS_NULLS;
            } else {
                const uint8_t *lp, *p;
                if (!pq_take(&c->values,4,&lp)) return "truncated string length";
                uint32_t len = pq_u32(lp);
                if (!pq_take(&c->values,len,&p)) return "truncated string data";
                if (s->strict && (!len || (s->converted == 0 && !pq_utf8(p,len))))
                    return "nonnull text is empty or invalid UTF-8";
                strings[count] = (const char*)p; lengths[count] = len;
                hashes[count] = ray_hash_bytes(p,len); positions[count++] = at+i;
            }
        }
        if (count) {
            if (!ray_sym_domain_intern_batch64(s->import_domain,count,strings,lengths,hashes,ids)) return "symbol domain allocation failed";
            for (int64_t i = 0; i < count; i++) {
                if (ids[i] < 0 || (uint64_t)ids[i] >= UINT32_MAX) return "symbol domain exceeds W32";
                dst[positions[i]] = (uint32_t)ids[i]; if (!ids[i]) v->attrs |= RAY_ATTR_HAS_NULLS;
            }
        }
        at += n; c->page_left -= n; c->chunk_left -= n;
        if (!c->page_left) {
            if (c->optional && !pq_rle_done(&c->defs)) return "excess definition levels";
            if (c->expected_nulls >= 0 && c->page_nulls != c->expected_nulls) return "v2 null count mismatch";
            if (c->encoding) { if (!pq_rle_done(&c->ids)) return "excess dictionary indices"; }
            else if (c->values.p != c->values.end) return "excess string values";
            if (!c->chunk_left && c->chunk.p != c->chunk.end) return "excess column pages";
        }
    }
    return NULL;
}
static const char* pq_decode(pq_column* c, pq_schema* s, ray_t** out, int64_t rows) {
    if (s->import_domain) return pq_decode_symbols(c,s,out,rows);
    ray_t* v = ray_vec_new((int8_t)s->type,rows);
    if (!v || RAY_IS_ERR(v)) { if (v) ray_release(v); return "vector allocation failed"; }
    v->len = rows; *out = v;
    if (v->type == RAY_STR) memset(ray_data(v),0,(size_t)rows*sizeof(ray_str_t));
    for (int64_t i = 0; i < rows; i++) {
        if (!(i&1023) && ray_interrupted()) return "scan interrupted";
        if (!c->page_left) { const char* err = pq_page(c,s,NULL); if (err) return err; }
        if (!c->optional && c->encoding && c->ids.left && !c->ids.packed) {
            uint32_t id = c->ids.value;
            if (id >= c->dict_count) return "dictionary index out of range";
            int64_t n = rows-i < c->page_left ? rows-i : c->page_left;
            if ((uint64_t)n > c->ids.left) n = (int64_t)c->ids.left;
            if (n > 4096) n = 4096;
            const uint8_t* p = s->physical == 6 ? c->strings[id].p :
                c->dict+(size_t)id*((s->physical == 1 || s->physical == 4) ? 4 : 8);
            uint32_t len = s->physical == 6 ? c->strings[id].n : 0;
            if (len > PQ_MAX_PAGE || (s->physical == 6 && v->str_pool && (uint64_t)v->str_pool->len+len > PQ_POOL_MAX)) return PQ_PUT_FAIL;
            const char* perr = pq_put_value(&v,i,s,p,len);
            if (perr) return perr;
            *out = v;
            /* Convert once, fill native values. STR descriptors may safely
             * share the same immutable bytes within this batch's pool. */
            if (ray_elem_size(v->type) == 2) {
                int16_t* dst = ray_data(v); int16_t x = dst[i];
                for (int64_t j = 1; j < n; j++) dst[i+j] = x;
            } else if (ray_elem_size(v->type) == 4) {
                uint32_t* dst = ray_data(v); uint32_t x; memcpy(&x,dst+i,4);
                for (int64_t j = 1; j < n; j++) memcpy(dst+i+j,&x,4);
            } else if (ray_elem_size(v->type) == 8) {
                uint64_t* dst = ray_data(v); uint64_t x; memcpy(&x,dst+i,8);
                for (int64_t j = 1; j < n; j++) memcpy(dst+i+j,&x,8);
            } else {
                size_t width = ray_elem_size(v->type); uint8_t* dst = ray_data(v);
                for (int64_t j = 1; j < n; j++) memcpy(dst+(size_t)(i+j)*width,dst+(size_t)i*width,width);
            }
            c->ids.left -= (uint64_t)n; c->page_left -= n-1; c->chunk_left -= n-1; i += n-1;
        } else if (!c->optional && !c->encoding &&
            ((s->physical == 1 && s->type == RAY_I32 && (s->converted == -1 || s->converted == 17)) ||
             (s->physical == 2 && s->type == RAY_I64))) {
            int64_t n = rows-i < c->page_left ? rows-i : c->page_left;
            if (n > 4096) n = 4096; /* Keep cancellation responsive. */
            const uint8_t* p;
            if (!pq_take(&c->values,(uint64_t)n*(s->physical == 1 ? 4 : 8),&p)) return "truncated numeric data";
            pq_plain_int(v,i,p,n);
            if (s->strict && (v->attrs & RAY_ATTR_HAS_NULLS)) return "nonnull integer collides with native null";
            c->page_left -= n-1; c->chunk_left -= n-1; i += n-1;
        } else {
            uint32_t present = 1;
            if (c->optional && (!pq_rle_next(&c->defs,&present) || present > 1)) return "invalid definition levels";
            if (!present) {
                if (ray_vec_set_null_checked(v,i,true) != RAY_OK) return "unrepresentable null";
                c->page_nulls++;
            } else {
                const uint8_t* p; uint32_t len = 0; uint8_t b = 0;
                if (c->encoding) {
                    uint32_t id;
                    if (!pq_rle_next(&c->ids,&id) || id >= c->dict_count) return "dictionary index out of range";
                    if (s->physical == 6) { p = c->strings[id].p; len = c->strings[id].n; }
                    else p = c->dict+(size_t)id*((s->physical == 1 || s->physical == 4) ? 4 : 8);
                } else if (c->bool_rle) {
                    uint32_t bit;
                    if (!pq_rle_next(&c->ids,&bit)) return "truncated RLE boolean data";
                    b = (uint8_t)bit; p = &b;
                } else if (s->physical == 0) {
                    if (c->bool_bit/8 >= (uint64_t)(c->values.end-c->values.p)) return "truncated boolean data";
                    b = (c->values.p[c->bool_bit/8]>>(c->bool_bit&7))&1; c->bool_bit++; p = &b;
                } else if (s->physical == 6) {
                    const uint8_t* lp;
                    if (!pq_take(&c->values,4,&lp)) return "truncated string length";
                    len = pq_u32(lp);
                    if (!pq_take(&c->values,len,&p)) return "truncated string data";
                } else if (!pq_take(&c->values,(s->physical == 1 || s->physical == 4) ? 4 : 8,&p)) return "truncated numeric data";
                if (s->physical == 6 && v->str_pool && (uint64_t)v->str_pool->len+len > PQ_POOL_MAX) return "batch string pool exceeds 4 GiB";
                if (len > PQ_MAX_PAGE) return PQ_PUT_FAIL;
                const char* perr = pq_put_value(&v,i,s,p,len);
                if (perr) return perr;
                *out = v;
            }
        }
        c->page_left--; c->chunk_left--;
        if (!c->page_left) {
            if (c->optional && !pq_rle_done(&c->defs)) return "excess definition levels";
            if (c->expected_nulls >= 0 && c->page_nulls != c->expected_nulls) return "v2 null count mismatch";
            if (c->encoding) { if (!pq_rle_done(&c->ids)) return "excess dictionary indices"; }
            else if (c->bool_rle) { if (!pq_rle_done(&c->ids)) return "excess RLE boolean data"; }
            else if (s->physical == 0) { if ((c->bool_bit+7)/8 != (uint64_t)(c->values.end-c->values.p)) return "excess boolean data"; }
            else if (c->values.p != c->values.end) return "excess plain data";
            if (!c->chunk_left && c->chunk.p != c->chunk.end) return "excess column pages";
        }
    }
    return NULL;
}
static const char* pq_discard(pq_column* c, pq_schema* s, int64_t rows) {
    while (rows) {
        if (ray_interrupted()) return "scan interrupted";
        if (!c->page_left) {
            const char* err = pq_page(c,s,&rows);
            if (err) return err;
            if (!rows) break;
        }
        int64_t n = rows < c->page_left ? rows : c->page_left;
        if (n > PQ_BATCH) n = PQ_BATCH;
        ray_t* discard = NULL;
        const char* err = pq_decode(c,s,&discard,n);
        if (discard) ray_release(discard);
        if (err) return err;
        rows -= n;
    }
    return NULL;
}
typedef struct {
    ray_parquet_t* reader;
    ray_t** columns;
    const char** errors;
    int64_t rows;
} pq_work;
static const char* pq_decode_column(ray_parquet_t* r, int64_t i, ray_t** out, int64_t rows) {
    pq_schema schema = r->schema[r->selected[i]];
    /* Filter before interning: rejected text must not grow the query domain. */
    if (r->filter_pos >= 0 || r->text_pattern) schema.import_domain = NULL;
    return pq_decode(&r->cursors[i],&schema,out,rows);
}
static const char* pq_intern_selected(pq_column* c, ray_t** vector, ray_sym_domain_t* domain) {
    pq_symbol_scratch* scratch = pq_symbols(c);
    if (!scratch) return "symbol scratch allocation failed";
    ray_t* src = *vector;
    ray_t* dst = ray_sym_vec_new(RAY_SYM_W32,src->len);
    if (!dst || RAY_IS_ERR(dst)) { if (dst) ray_release(dst); return "symbol allocation failed"; }
    dst->sym_domain = domain; ray_sym_domain_retain(domain); dst->len = src->len;
    const char** strings = scratch->strings; size_t* lengths = scratch->lengths;
    uint64_t* hashes = scratch->hashes; int64_t* ids = scratch->ids;
    for (int64_t at = 0; at < src->len; at += 8192) {
        int64_t n = src->len-at < 8192 ? src->len-at : 8192;
        for (int64_t i = 0; i < n; i++) {
            strings[i] = ray_str_vec_get(src,at+i,&lengths[i]);
            hashes[i] = ray_hash_bytes(strings[i],lengths[i]);
        }
        if (!ray_sym_domain_intern_batch64(domain,n,strings,lengths,hashes,ids)) {
            ray_release(dst); return "symbol domain allocation failed";
        }
        for (int64_t i = 0; i < n; i++) {
            if (ids[i] < 0 || (uint64_t)ids[i] >= UINT32_MAX) { ray_release(dst); return "symbol domain exceeds W32"; }
            ((uint32_t*)ray_data(dst))[at+i] = (uint32_t)ids[i];
            if (!ids[i]) dst->attrs |= RAY_ATTR_HAS_NULLS;
        }
    }
    ray_release(src); *vector = dst; return NULL;
}
static void pq_decode_task(void* ptr, uint32_t worker, int64_t start, int64_t end) {
    (void)worker;
    pq_work* w = ptr; ray_parquet_t* r = w->reader;
    for (int64_t i = start; i < end; i++) if (!w->columns[i])
        w->errors[i] = pq_decode_column(r,i,&w->columns[i],w->rows);
}
int64_t ray_parquet_pages_skipped(const ray_parquet_t* r) {
    int64_t n = 0;
    if (r) for (int64_t i = 0; i < r->nselected; i++) n += r->cursors[i].pages_skipped;
    return n;
}
int64_t ray_parquet_bloom_skipped(const ray_parquet_t* r) { return r ? r->bloom_skipped : 0; }
int64_t ray_parquet_parallel_batches(const ray_parquet_t* r) { return r ? r->parallel_batches : 0; }

ray_t* ray_parquet_next(ray_parquet_t* r) {
    if (!r || r->failed) return pq_error("reader is closed or failed");
    if (ray_interrupted()) { r->failed = true; return ray_error("cancel","parquet scan interrupted"); }
    const char* err = NULL;
    int64_t rows;
    for (;;) {
        while (!r->group_left && r->group+1 < r->ngroups) {
            r->group++;
            err = pq_start_group(r);
            if (err) { r->failed = true; return pq_error(err); }
        }
        if (!r->group_left && r->emitted) return NULL;
        int64_t cap = r->group_batch > 0 ? r->group_batch : r->batch_rows;
        rows = r->group_left < cap ? r->group_left : cap;
        if (r->group_left && r->exclude_pos < r->nexcluded) {
            pq_interval range = r->excluded[r->exclude_pos];
            int64_t at = r->group_rows-r->group_left;
            if (at == range.lo) {
                for (int64_t i = 0; i < r->nselected; i++) {
                    pq_schema schema = r->schema[r->selected[i]]; schema.import_domain = NULL;
                    err = pq_discard(&r->cursors[i],&schema,range.hi-range.lo);
                    if (err) { r->failed = true; return pq_error(err); }
                }
                r->group_left -= range.hi-range.lo; r->exclude_pos++;
                continue;
            }
            if (rows > range.lo-at) rows = range.lo-at;
        }
        break;
    }
    ray_t* tbl = ray_table_new(r->noutput);
    if (!tbl || RAY_IS_ERR(tbl)) return tbl ? tbl : ray_error("oom",NULL);
    ray_t** cols = ray_calloc_raw((size_t)r->nselected*sizeof(*cols));
    uint8_t* keep = NULL;
    const char** errors = NULL;
    int64_t bad_col = -1;
    if (!cols) { ray_release(tbl); r->failed = true; return ray_error("oom",NULL); }
    ray_pool_t* pool = rows >= 4096 && r->nselected > 1 ? ray_pool_get() : NULL;
    if (ray_pool_par_dispatch_ok(pool,rows,4096)) {
        errors = ray_calloc_raw((size_t)r->nselected*sizeof(*errors));
        if (!errors) { err = "task allocation failed"; goto fail; }
        pq_work work = {r,cols,errors,rows};
        ray_pool_dispatch_n(pool,pq_decode_task,&work,(uint32_t)r->nselected);
        r->parallel_batches++;
        for (int64_t i = 0; i < r->nselected; i++) {
            if (errors[i] || !cols[i]) { err = errors[i] ? errors[i] : "scan interrupted"; bad_col = i; goto fail; }
        }
    } else for (int64_t i = 0; i < r->nselected; i++) {
        err = pq_decode_column(r,i,&cols[i],rows);
        if (err) { bad_col = i; goto fail; }
    }
    int64_t kept = rows;
    if (r->filter_pos >= 0 && rows) {
        keep = ray_alloc_raw((size_t)rows);
        if (!keep) { err = "selection allocation failed"; goto fail; }
        ray_t* f = cols[r->filter_pos]; kept = 0;
        for (int64_t i = 0; i < rows; i++) {
            int64_t value = f->type == RAY_I16 ? ((int16_t*)ray_data(f))[i] :
                            f->type == RAY_I32 ? ((int32_t*)ray_data(f))[i] : ((int64_t*)ray_data(f))[i];
            keep[i] = ray_vec_is_null(f,i) ? r->filter_nulls : value >= r->filter_lo && value <= r->filter_hi;
            kept += keep[i];
        }
    }
    if (r->text_pattern && rows) {
        if (!keep) keep = ray_alloc_raw((size_t)rows);
        if (!keep) { err = "selection allocation failed"; goto fail; }
        const char* pat = ray_str_ptr(r->text_pattern); size_t pn = ray_str_len(r->text_pattern);
        ray_glob_compiled_t compiled = ray_glob_compile(pat,pn);
        kept = 0;
        for (int64_t i = 0; i < rows; i++) {
            size_t len; const char* str = ray_str_vec_get(cols[r->text_pos],i,&len);
            keep[i] = compiled.shape == RAY_GLOB_SHAPE_NONE ? ray_glob_match(str,len,pat,pn) :
                ray_glob_match_compiled(&compiled,str,len);
            kept += keep[i];
        }
    }
    for (int64_t c = 0; c < r->noutput; c++) {
        ray_t* v = cols[c];
        if (kept != rows) {
            size_t esz = ray_sym_elem_size(v->type,v->attrs); uint8_t* data = ray_data(v); int64_t dst = 0;
            for (int64_t i = 0; i < rows; i++) if (keep[i]) {
                if (dst != i) memcpy(data+(size_t)dst*esz,data+(size_t)i*esz,esz);
                dst++;
            }
            v->len = kept;
        }
        if (v->type == RAY_STR && r->schema[r->selected[c]].import_domain) {
            err = pq_intern_selected(&r->cursors[c],&cols[c],r->schema[r->selected[c]].import_domain);
            if (err) goto fail;
            v = cols[c];
        }
        tbl = ray_table_add_col(tbl,r->schema[r->selected[c]].name,v);
        if (!tbl || RAY_IS_ERR(tbl)) { err = "table allocation failed"; goto fail; }
    }
    r->group_left -= rows; r->emitted = true;
    for (int64_t c = 0; c < r->nselected; c++) ray_release(cols[c]);
    ray_free_raw(cols); ray_free_raw(keep); ray_free_raw(errors);
    return tbl;
fail:
    for (int64_t c = 0; c < r->nselected; c++) if (cols[c]) ray_release(cols[c]);
    ray_free_raw(cols); ray_free_raw(keep); ray_free_raw(errors);
    if (tbl) ray_release(tbl);
    r->failed = true;
    if (ray_interrupted()) return ray_error("cancel","parquet scan interrupted");
    if (bad_col >= 0) {
        ray_t* name = ray_sym_str(r->schema[r->selected[bad_col]].name);
        return ray_error("parquet","%s (column %s)",err,name ? ray_str_ptr(name) : "?");
    }
    return pq_error(err);
}

/* Optional Rayfall scan settings: {columns: [x y] range: {column: k min: 1 max: 9}}.
 * The C cursor also exposes the same range directly, without language objects. */
static ray_t* pq_option(ray_t* options, const char* name) {
    ray_t* key = ray_sym(ray_sym_intern(name,strlen(name)));
    if (!key || RAY_IS_ERR(key)) return key;
    ray_t* result = ray_dict_get(options,key); ray_release(key); return result;
}
static ray_t* pq_import_types(ray_parquet_t* r, ray_t* types);
static ray_t* pq_strict_option(ray_parquet_t* r, ray_t* options) {
    ray_t* strict = pq_option(options,"strict");
    if (!strict) return NULL;
    bool valid = strict->type == -RAY_BOOL;
    if (valid) for (int64_t c = 0; c < r->ncols; c++) r->schema[c].strict = strict->b8 != 0;
    ray_release(strict);
    return valid ? NULL : pq_error("strict must be a boolean");
}
static ray_t* pq_scan_types(ray_parquet_t* r, ray_t* types) {
    if (types && types->type != RAY_SYM) return pq_error("types must be a symbol vector");
    ray_t* err = pq_import_types(r,types);
    if (err) return err;
    for (int64_t c = 0; c < r->ncols; c++) if (r->schema[c].native_symbol) {
        if (!r->scan_domain) r->scan_domain = ray_sym_domain_new();
        if (!r->scan_domain) return ray_error("oom",NULL);
        r->schema[c].import_domain = r->scan_domain;
    }
    return NULL;
}
static ray_t* pq_open_options(const char* path, ray_t* options, ray_parquet_t** out) {
    if (!options || options->type != RAY_DICT) return ray_parquet_open(path,options,PQ_BATCH,out);
    *out = NULL;
    ray_t* keys = ray_dict_keys(options);
    if (!keys || keys->type != RAY_SYM) return pq_error("scan option keys must be symbols");
    for (int64_t i = 0; i < keys->len; i++) {
        ray_t* key = ray_sym_vec_cell(keys,i); const char* k = key ? ray_str_ptr(key) : NULL;
        if (!k || (strcmp(k,"columns") && strcmp(k,"range") && strcmp(k,"types") && strcmp(k,"strict"))) return pq_error("unknown scan option");
    }
    ray_t* columns = pq_option(options,"columns");
    ray_t* range = pq_option(options,"range");
    ray_t* types = pq_option(options,"types");
    ray_t* err = NULL;
    if ((columns && RAY_IS_ERR(columns)) || (range && RAY_IS_ERR(range))) { err = pq_error("invalid scan options"); goto done; }
    err = ray_parquet_open(path,columns,PQ_BATCH,out);
    if (!err) err = pq_scan_types(*out,types);
    if (!err) err = pq_strict_option(*out,options);
    if (err || !range) goto done;
    if (range->type != RAY_DICT) { err = pq_error("range must be {column: name min: lo max: hi}"); goto done; }
    ray_t* name = pq_option(range,"column"); ray_t* lo = pq_option(range,"min"); ray_t* hi = pq_option(range,"max");
    if (!name || !lo || !hi || name->type != -RAY_SYM || lo->type != -RAY_I64 || hi->type != -RAY_I64)
        err = pq_error("range expects a symbol column and two I64 bounds");
    else err = ray_parquet_range(*out,name->i64,lo->i64,hi->i64);
    if (name) ray_release(name);
    if (lo) ray_release(lo);
    if (hi) ray_release(hi);
done:
    if (columns) ray_release(columns);
    if (range) ray_release(range);
    if (types) ray_release(types);
    if (err && *out) { ray_parquet_close(*out); *out = NULL; }
    return err;
}

/* Row-group tasks share only immutable mapping/schema data. Each task owns its
 * cursors, page buffers, dictionary and selection. The parent outlives the join.
 * Cursors for the first `nselected` of the parent's selected columns (all of
 * them, or the one column a direct import task sets). */
static ray_parquet_t* pq_group_reader(const ray_parquet_t* parent, int64_t group, int64_t nselected) {
    ray_parquet_t* r = ray_calloc_raw(sizeof(*r));
    if (!r) return NULL;
    r->borrowed = true; r->map = parent->map; r->size = parent->size; r->data_end = parent->data_end;
    r->schema = parent->schema; r->groups = parent->groups; r->ncols = parent->ncols;
    r->ngroups = group+1; r->group = group-1; r->batch_rows = parent->batch_rows;
    r->nselected = nselected; r->noutput = parent->noutput;
    r->filter_pos = parent->filter_pos; r->filter_lo = parent->filter_lo; r->filter_hi = parent->filter_hi;
    r->filter_nulls = parent->filter_nulls;
    r->text_pattern = parent->text_pattern; r->text_pos = parent->text_pos;
    r->selected = ray_alloc_raw((size_t)r->nselected*sizeof(*r->selected));
    r->cursors = ray_calloc_raw((size_t)r->nselected*sizeof(*r->cursors));
    if (!r->selected || !r->cursors) { ray_parquet_close(r); return NULL; }
    memcpy(r->selected,parent->selected,(size_t)r->nselected*sizeof(*r->selected));
    return r;
}
static ray_pool_t* pq_group_pool(ray_parquet_t* r) {
    ray_pool_t* pool = r->ngroups > 1 ? ray_pool_get() : NULL;
    return ray_pool_par_dispatch_ok(pool,r->ngroups,2) ? pool : NULL;
}
static ray_t* pq_append_table(ray_t** vectors, ray_t* batch, int64_t ncols) {
    int64_t nr = ray_table_nrows(batch);
    for (int64_t c = 0; c < ncols; c++) {
        ray_t* src = ray_table_get_col_idx(batch,c);
        if (src->type == RAY_STR) {
            for (int64_t i = 0; i < nr; i++) {
                size_t n; const char* p = ray_str_vec_get(src,i,&n);
                ray_t* v = ray_str_vec_append(vectors[c],p,n);
                if (!v || RAY_IS_ERR(v)) return v ? v : ray_error("oom",NULL);
                vectors[c] = v;
            }
        } else {
            ray_t* v = ray_vec_append_raw(vectors[c],ray_data(src),nr);
            if (!v || RAY_IS_ERR(v)) return v ? v : ray_error("oom",NULL);
            vectors[c] = v;
        }
        vectors[c]->attrs |= src->attrs & RAY_ATTR_HAS_NULLS;
    }
    return NULL;
}
static ray_t* pq_materialize(ray_parquet_t* r);
typedef struct { ray_parquet_t* parent; int64_t first; ray_t** tables; } pq_read_work;
static void pq_read_group(void* ptr, uint32_t worker, int64_t start, int64_t end) {
    (void)worker; pq_read_work* w = ptr;
    for (int64_t i = start; i < end; i++) {
        ray_parquet_t* r = pq_group_reader(w->parent,w->first+i,w->parent->nselected);
        w->tables[i] = r ? pq_materialize(r) : ray_error("oom",NULL);
    }
}
static ray_t* pq_materialize(ray_parquet_t* r) {
    ray_t* err = NULL;
    ray_t** vectors = ray_calloc_raw((size_t)r->noutput*sizeof(*vectors));
    if (!vectors) { ray_parquet_close(r); return ray_error("oom",NULL); }
    for (int64_t c = 0; c < r->noutput; c++) {
        pq_schema* schema = &r->schema[r->selected[c]];
        vectors[c] = schema->import_domain ? ray_sym_vec_new(RAY_SYM_W32,0) : ray_vec_new((int8_t)schema->type,0);
        if (!vectors[c] || RAY_IS_ERR(vectors[c])) {
            err = vectors[c] ? vectors[c] : ray_error("oom",NULL); vectors[c] = NULL; goto done;
        }
        if (schema->import_domain) {
            vectors[c]->sym_domain = schema->import_domain;
            ray_sym_domain_retain(schema->import_domain);
        }
    }
    ray_pool_t* pool = pq_group_pool(r);
    if (pool) {
        /* Bound temporary materialized output to one wave, retain source order. */
        uint32_t width = ray_pool_total_workers(pool);
        if (width > RAY_POOL_INIT_TASKS) width = RAY_POOL_INIT_TASKS;
        ray_t** tables = ray_calloc_raw((size_t)width*sizeof(*tables));
        if (!tables) { err = ray_error("oom",NULL); goto done; }
        for (int64_t first = 0; first < r->ngroups && !err; first += width) {
            uint32_t n = r->ngroups-first < width ? (uint32_t)(r->ngroups-first) : width;
            memset(tables,0,(size_t)n*sizeof(*tables));
            pq_read_work work = {r,first,tables};
            ray_pool_dispatch_n(pool,pq_read_group,&work,n);
            for (uint32_t i = 0; i < n; i++) {
                ray_t* batch = tables[i];
                if (!err) {
                    if (!batch) err = ray_error("cancel","parquet scan interrupted");
                    else if (RAY_IS_ERR(batch)) { err = batch; batch = NULL; }
                    else err = pq_append_table(vectors,batch,r->noutput);
                }
                if (batch) ray_release(batch);
            }
        }
        ray_free_raw(tables);
        if (err) goto done;
    } else for (;;) {
        ray_t* batch = ray_parquet_next(r);
        if (!batch) break;
        if (RAY_IS_ERR(batch)) { err = batch; goto done; }
        err = pq_append_table(vectors,batch,r->noutput);
        ray_release(batch);
        if (err) goto done;
    }
    err = ray_table_new(r->noutput);
    for (int64_t c = 0; c < r->noutput && err && !RAY_IS_ERR(err); c++)
        err = ray_table_add_col(err,r->schema[r->selected[c]].name,vectors[c]);
done:
    for (int64_t c = 0; c < r->noutput; c++) if (vectors[c]) ray_release(vectors[c]);
    ray_free_raw(vectors); ray_parquet_close(r);
    return err ? err : ray_error("oom",NULL);
}

ray_t* ray_parquet_read(const char* path, ray_t* columns) {
    ray_parquet_t* r = NULL;
    ray_t* err = pq_open_options(path,columns,&r);
    return err ? err : pq_materialize(r);
}

ray_t* ray_parquet_metadata(const char* path) {
    ray_parquet_t* r = NULL; ray_t* err = ray_parquet_open(path,NULL,PQ_BATCH,&r);
    if (err) return err;
    const char* names[] = {"row_group","column","physical_type","ray_type","codec","rows","compressed_bytes","uncompressed_bytes","statistics","page_index","bloom_filter"};
    enum { N = 11 };
    ray_t* v[N] = {0}; pq_span* cols = NULL;
    int64_t count = r->ngroups*r->ncols;
    if ((uint64_t)count > PQ_MAX_FOOTER/(N*sizeof(int64_t))) {
        ray_parquet_close(r); return pq_error("metadata result exceeds 64 MiB");
    }
    for (int i = 0; i < N; i++) {
        v[i] = i == 1 || i == 3 ? ray_sym_vec_new(RAY_SYM_W64,count) : ray_vec_new(RAY_I64,count);
        if (!v[i] || RAY_IS_ERR(v[i])) { err = v[i]; v[i] = NULL; goto done; }
        v[i]->len = count;
    }
    cols = ray_alloc_raw((size_t)r->ncols*sizeof(*cols));
    if (!cols) goto done;
    for (int64_t g = 0; g < r->ngroups; g++) {
        int64_t nr;
        if (!pq_group_columns(r,g,cols,&nr,NULL)) { err = pq_error("invalid row group metadata"); goto done; }
        pq_span gf[8];
        if (!pq_fields(r->groups[g],gf,8)) { err = pq_error("invalid row group"); goto done; }
        pq_cur cc = {gf[1].p,gf[1].end,false}; uint8_t ct; pq_list(&cc,&ct);
        for (int64_t c = 0; c < r->ncols; c++) {
            int64_t row = g*r->ncols+c; pq_span mf[17], ch, cf[10];
            if (!pq_fields(cols[c],mf,17) || !pq_element(&cc,12,&ch) || !pq_fields(ch,cf,10)) {
                err = pq_error("invalid column metadata"); goto done;
            }
            const char* tn = ray_type_name((int8_t)r->schema[c].type);
            int64_t cells[N] = {g,r->schema[c].name,r->schema[c].physical,ray_sym_intern(tn,strlen(tn)),
                pq_get(mf[4],-1),nr,pq_get(mf[7],-1),pq_get(mf[6],-1),!!mf[12].type,
                !!cf[4].type && !!cf[6].type,!!mf[14].type};
            for (int i = 0; i < N; i++) ((int64_t*)ray_data(v[i]))[row] = cells[i];
        }
    }
    err = ray_table_new(N);
    for (int i = 0; i < N && err && !RAY_IS_ERR(err); i++)
        err = ray_table_add_col(err,ray_sym_intern(names[i],strlen(names[i])),v[i]);
done:
    for (int i = 0; i < N; i++) if (v[i]) ray_release(v[i]);
    ray_free_raw(cols); ray_parquet_close(r);
    return err ? err : ray_error("oom",NULL);
}

static bool pq_safe_name(const char* s) {
    if (!s || !*s || *s == '.') return false;
    for (; *s; s++) if (*s == '/' || *s == '\\' || *s == ':') return false;
    return true;
}
/* Import schemas are positional, like CSV schemas. Unannotated integer dates
 * and timestamps require explicit UNIX_* units rather than guessing epochs. */
static ray_t* pq_import_types(ray_parquet_t* r, ray_t* types) {
    if (!types) return NULL;
    if (types->type == RAY_DICT) {
        ray_t* keys = ray_dict_keys(types);
        if (!keys || keys->type != RAY_SYM) return pq_error("import option keys must be symbols");
        for (int64_t i = 0; i < keys->len; i++) {
            ray_t* key = ray_sym_vec_cell(keys,i); const char* k = key ? ray_str_ptr(key) : NULL;
            if (!k || (strcmp(k,"types") && strcmp(k,"strict") && strcmp(k,"rows"))) return pq_error("unknown import option");
        }
        ray_t* err = pq_strict_option(r,types);
        ray_t* rows = pq_option(types,"rows");
        if (!err && rows) {
            if (rows->type != -RAY_I64 || rows->i64 < 1 || rows->i64 > PQ_MAX_PART_ROWS)
                err = pq_error("partition rows must be 1..1048576");
            else r->batch_rows = rows->i64;
        }
        if (rows) ray_release(rows);
        ray_t* schema = pq_option(types,"types");
        if (!err && schema && schema->type != RAY_SYM) err = pq_error("types must be a symbol vector");
        if (!err) err = pq_import_types(r,schema);
        if (schema) ray_release(schema);
        return err;
    }
    if (types->type != RAY_SYM || types->len != r->ncols) return pq_error("import types must match the Parquet columns");
    for (int64_t c = 0; c < r->ncols; c++) {
        ray_t* atom = ray_sym_vec_cell(types,c); const char* name = atom ? ray_str_ptr(atom) : NULL;
        pq_schema* s = &r->schema[c];
        if (!name) return pq_error("invalid import type");
        if (!strcmp(name,"SYM") && s->type == RAY_STR) { s->native_symbol = true; continue; }
        if (!strcmp(name,ray_type_name((int8_t)s->type))) continue;
        bool integer = (s->type == RAY_I16 || s->type == RAY_I32 || s->type == RAY_I64);
        if (!integer) return pq_error("unsupported import type conversion");
        if (!strcmp(name,"I16")) s->type = RAY_I16;
        else if (!strcmp(name,"I32")) s->type = RAY_I32;
        else if (!strcmp(name,"I64")) s->type = RAY_I64;
        else if (!strcmp(name,"UNIX_DATE")) s->type = RAY_DATE;
        else {
            int64_t scale = !strcmp(name,"UNIX_SECONDS") ? 1000000000 :
                !strcmp(name,"UNIX_MILLIS") ? 1000000 : !strcmp(name,"UNIX_MICROS") ? 1000 :
                !strcmp(name,"UNIX_NANOS") ? 1 : 0;
            if (!scale) return pq_error("unsupported import type; raw integer timestamps require explicit UNIX_* units");
            s->type = RAY_TIMESTAMP; s->scale = scale;
        }
    }
    return NULL;
}
typedef struct {
    ray_parquet_t* parent;
    const char *root, *table;
    int64_t *offsets;
    ray_t** errors;
    bool durable;
} pq_native_work;
/* These partitions are private to the import staging directory. Persist the
 * shared vocabulary once after all workers join, before publishing the root.
 * Flushing it here rewrites the growing dictionary for every partition. */
static ray_err_t pq_save_partition(ray_parquet_t* r, ray_t* batch, const char* leaf,
                                   const char* sym, bool durable) {
    ray_sym_domain_t* domain = NULL;
    for (int64_t c = 0; c < r->ncols; c++) if (r->schema[c].import_domain) domain = r->schema[c].import_domain;
    /* No import domain means no SYM column, so the vocabulary is untouched.
     * The non-durable callers (the splayed spool and parted partitions) write
     * private leaves under the import's staging root, published by renaming
     * the whole tree: write them as a bare staged save, without the writer
     * lock, generation handling or index pass that publishing a live table
     * needs.  The spool then deletes exactly the files it wrote. */
    if (!domain) return durable ? ray_splay_save(batch,leaf,sym) : ray_splay_save_staged_bulk(batch,leaf,sym);
    ray_err_t e = ray_mkdir_p(leaf); if (e != RAY_OK) return e;
    ray_t* schema = ray_vec_new(RAY_STR,r->ncols);
    if (!schema || RAY_IS_ERR(schema)) { if (schema) ray_release(schema); return RAY_ERR_OOM; }
    char path[1600];
    for (int64_t c = 0; c < r->ncols; c++) {
        ray_t* name = ray_sym_str(r->schema[c].name);
        int n = snprintf(path,sizeof(path),"%s/%s",leaf,ray_str_ptr(name));
        if (n < 0 || (size_t)n >= sizeof(path)) { e = RAY_ERR_RANGE; break; }
        ray_t* col = ray_table_get_col_idx(batch,c);
        e = col->type == RAY_SYM ? ray_col_save_sym_encoded(col,path,domain,durable) :
            durable ? ray_col_save(col,path) : ray_col_save_bulk(col,path);
        if (e != RAY_OK) break;
        ray_t* next = ray_str_vec_append(schema,ray_str_ptr(name),ray_str_len(name));
        if (!next || RAY_IS_ERR(next)) { if (next) ray_release(next); e = RAY_ERR_OOM; break; }
        schema = next;
    }
    if (e == RAY_OK) {
        int n = snprintf(path,sizeof(path),"%s/.d",leaf);
        e = n < 0 || (size_t)n >= sizeof(path) ? RAY_ERR_RANGE :
            durable ? ray_col_save(schema,path) : ray_col_save_bulk(schema,path);
    }
    ray_release(schema); return e;
}
static ray_t* pq_write_reader(ray_parquet_t* r, const char* root, const char* table,
                              int64_t part, bool durable) {
    char sym[1200]; snprintf(sym,sizeof(sym),"%s/.sym",root);
    for (;;) {
        ray_t* batch = ray_parquet_next(r);
        if (!batch || RAY_IS_ERR(batch)) return batch;
        /* Native zones are built while the bounded decoded batch is resident. */
        if (durable) for (int64_t c = 0; c < r->ncols; c++) {
            ray_t* col = ray_table_get_col_idx(batch,c);
            if (col->type == RAY_STR || col->type == RAY_SYM || col->type == RAY_F32) continue;
            ray_retain(col);
            ray_t* e = col->len >= PQ_BATCH ? ray_index_attach_chunk_zone(&col,16) : ray_index_attach_zone(&col);
            if (e && RAY_IS_ERR(e)) { ray_release(col); ray_release(batch); return e; }
            ray_table_set_col_idx(batch,c,col); ray_release(col);
        }
        char leaf[1400];
        int n = snprintf(leaf,sizeof(leaf),"%s/%lld/%s",root,(long long)part++,table);
        /* All leaves are private staging output. Sync them together after
         * the workers finish, avoiding a directory fsync per column rename. */
        ray_err_t e = n < 0 || (size_t)n >= sizeof(leaf) ? RAY_ERR_RANGE :
            pq_save_partition(r,batch,leaf,sym,false);
        ray_release(batch);
        if (e != RAY_OK) return ray_error(ray_err_code_str(e),"parquet: native partition write failed");
    }
}
static void pq_write_group(void* ptr, uint32_t worker, int64_t start, int64_t end) {
    (void)worker; pq_native_work* w = ptr;
    for (int64_t g = start; g < end; g++) {
        ray_parquet_t* r = pq_group_reader(w->parent,g,w->parent->nselected);
        if (!r) { w->errors[g] = ray_error("oom",NULL); continue; }
        w->errors[g] = pq_write_reader(r,w->root,w->table,w->offsets[g],w->durable);
        ray_parquet_close(r);
    }
}
static ray_t* pq_write_groups(ray_parquet_t* r, const char* root, const char* table,
                              bool durable, int64_t* parts) {
    *parts = 0;
    int64_t ng = r->ngroups;
    int64_t* offsets = ray_alloc_raw((size_t)(ng+1)*sizeof(*offsets));
    ray_t** errors = ray_calloc_raw((size_t)(ng+1)*sizeof(*errors));
    ray_t* err = NULL;
    if (!offsets || !errors) { err = ray_error("oom",NULL); goto done; }
    for (int64_t g = 0; g < ng; g++) {
        pq_span fields[8];
        if (!pq_fields(r->groups[g],fields,8)) { err = pq_error("invalid row group"); goto done; }
        int64_t rows = pq_get(fields[3],-1);
        if (rows < 0) { err = pq_error("invalid row group row count"); goto done; }
        offsets[g] = *parts;
        int64_t n = rows ? 1+(rows-1)/pq_group_batch(r,g) : 1;
        if (*parts > INT64_MAX-n) { err = pq_error("too many partitions"); goto done; }
        *parts += n;
    }
    if (!ng) { *parts = 1; err = pq_write_reader(r,root,table,0,durable); goto done; }
    pq_native_work work = {r,root,table,offsets,errors,durable};
    ray_pool_t* pool = pq_group_pool(r);
    if (pool) {
        /* dispatch uses a bounded ring; tasks claim groups dynamically. */
        ray_pool_dispatch_n(pool,pq_write_group,&work,(uint32_t)ng);
    } else pq_write_group(&work,0,0,ng);
    for (int64_t g = 0; g < ng; g++) if (errors[g]) {
        if (!err) err = errors[g]; else ray_error_free(errors[g]);
    }
    if (!err && ray_interrupted()) err = ray_error("cancel","parquet conversion interrupted");
done:
    ray_free_raw(offsets); ray_free_raw(errors); return err;
}
/* The destination without trailing separators, as .csv.parted takes it:
 * "out/" must stage in "out.parquet-partial", not "out/.parquet-partial". */
static bool pq_dest(const char* in, char* out, size_t size) {
    size_t len = in ? strlen(in) : 0;
    while (len > 1 && (in[len-1] == '/' || in[len-1] == '\\')) len--;
    if (!len || len >= size) return false;
    memcpy(out,in,len); out[len] = 0;
    return true;
}
static ray_t* pq_stage(ray_parquet_t* r, const char* root, char* staging, size_t size, bool parted) {
    struct stat st;
    int n = snprintf(staging,size,"%s.parquet-partial",root);
    if (n < 0 || (size_t)n >= size || stat(root,&st) == 0)
        return pq_error("destination exists or path is too long");
    for (int64_t c = 0; c < r->ncols; c++) {
        const char* name = ray_str_ptr(ray_sym_str(r->schema[c].name));
        if (!pq_safe_name(name) || (parted && !strcmp(name,"part")))
            return pq_error("column name is unsafe or conflicts with native partition key 'part'");
    }
#ifdef RAY_OS_WINDOWS
    bool created = CreateDirectoryA(staging,NULL) != 0;
#else
    bool created = mkdir(staging,0755) == 0;
#endif
    return created ? NULL : ray_error("parquet","cannot create staging directory %s (the parent must exist; one left by an earlier failed import is not overwritten, remove it to retry)",staging);
}
static ray_t* pq_publish(const char* staging, const char* root, int64_t rows) {
    struct stat st;
    if (ray_interrupted()) return ray_error("cancel","parquet conversion interrupted");
    char child[1100];
    int n = snprintf(child,sizeof(child),"%s/.d",staging);
    if (n < 0 || (size_t)n >= sizeof(child) || ray_file_sync_dir(child) != RAY_OK)
        return pq_error("cannot sync staging directory; native destination not published");
    if (stat(root,&st) == 0 || ray_file_rename_new(staging,root) != RAY_OK)
        return pq_error("cannot publish native destination; staging directory retained");
    if (ray_file_sync_dir(root) != RAY_OK) return pq_error("native destination published but directory sync failed");
    return ray_i64(rows);
}
static ray_t* pq_sync_partitions(ray_parquet_t* r, const char* root, const char* table, int64_t parts) {
    char path[1600], leaf[1400];
    for (int64_t part = 0; part < parts; part++) {
        if (ray_interrupted()) return ray_error("cancel","parquet conversion interrupted");
        int n = snprintf(leaf,sizeof(leaf),"%s/%lld/%s",root,(long long)part,table);
        if (n < 0 || (size_t)n >= sizeof(leaf)) return pq_error("partition path is too long");
        for (int64_t c = 0; c <= r->ncols; c++) {
            const char* name = c == r->ncols ? ".d" : ray_str_ptr(ray_sym_str(r->schema[c].name));
            n = snprintf(path,sizeof(path),"%s/%s",leaf,name);
            if (n < 0 || (size_t)n >= sizeof(path)) return pq_error("column path is too long");
            ray_fd_t fd = ray_file_open(path,RAY_OPEN_READ | RAY_OPEN_WRITE);
            if (fd == RAY_FD_INVALID) return pq_error("cannot open staged column for sync");
            ray_err_t e = ray_file_sync(fd); ray_file_close(fd);
            if (e != RAY_OK) return ray_error(ray_err_code_str(e),"parquet: staged column sync failed");
        }
        /* path names .d, so sync its parent (leaf), then leaf's parent
         * (partition). pq_publish persists the staging root before rename. */
        if (ray_file_sync_dir(path) != RAY_OK || ray_file_sync_dir(leaf) != RAY_OK)
            return pq_error("cannot sync partition directories");
    }
    return NULL;
}
ray_t* ray_parquet_parted_typed(const char* path, const char* root, const char* table, ray_t* types) {
#ifdef RAY_FUZZING
    return ray_error("restricted","Parquet imports disabled under fuzzing");
#endif
    if (!pq_safe_name(table)) return pq_error("invalid destination table name");
    char dest[1024];
    if (!pq_dest(root,dest,sizeof(dest))) return pq_error("invalid or too long destination path");
    root = dest;
    ray_parquet_t* r = NULL; ray_t* err = ray_parquet_open(path,NULL,PQ_BATCH,&r);
    if (err) return err;
    char staging[1100], sym[1200]; int64_t parts;
    ray_sym_domain_t* domain = NULL;
    err = pq_import_types(r,types);
    if (!err) err = pq_stage(r,root,staging,sizeof(staging),true);
    if (err) goto done;
    /* Global runtime interning is not worker-safe. Decode symbols straight
     * into the shared, synchronized file domain, as in direct splayed loads. */
    for (int64_t c = 0; c < r->ncols; c++) if (r->schema[c].native_symbol) {
        if (!domain) {
            snprintf(sym,sizeof(sym),"%s/.sym",staging);
            domain = ray_sym_domain_open_or_create(sym);
            if (!domain || ray_sym_domain_intern(domain,"",0) != 0) {
                err = ray_error("oom",NULL); goto done;
            }
        }
        r->schema[c].import_domain = domain;
    }
    err = pq_write_groups(r,staging,table,true,&parts);
    if (!err && domain) {
        /* Every partition header must be covered by the final vocabulary
         * before publication, including symbols added by other group tasks. */
        ray_err_t e = ray_sym_domain_flush(domain,true);
        if (e != RAY_OK) err = ray_error(ray_err_code_str(e),"parquet: cannot flush symbol file");
    }
    if (!err) err = pq_sync_partitions(r,staging,table,parts);
    if (!err) err = pq_publish(staging,root,r->rows);
done:
    if (domain) ray_sym_domain_release(domain);
    ray_parquet_close(r); return err;
}
ray_t* ray_parquet_parted(const char* path, const char* root, const char* table) {
    return ray_parquet_parted_typed(path,root,table,NULL);
}
static bool pq_remove_dir(const char* path) {
#ifdef RAY_OS_WINDOWS
    return RemoveDirectoryA(path) != 0;
#else
    return rmdir(path) == 0;
#endif
}
/* Column by column below this many times the output's bytes of memory
 * (see pq_write_direct). */
#define PQ_COLWISE_RATIO 2

typedef struct {
    ray_parquet_t* parent;
    ray_col_stream_t* writers;
    int64_t* offsets;
    _Atomic uint32_t* nulls;
    _Atomic uint32_t* locks;
    ray_t** errors;
    const int64_t* pass;           /* the columns of the current pass */
    int64_t npass;
    const int64_t* chunk_lo;       /* [ngroups * ncols] byte range of each column chunk, */
    const int64_t* chunk_hi;       /*   lo < 0 when the metadata does not give one */
    _Atomic uint8_t* prefetched;   /* [ngroups * ncols]: read-ahead already requested */
    int64_t prefetch;              /* row groups requested ahead of the one starting */
    int64_t ahead_bytes;           /* cap on what one task requests past its own chunk */
    bool writeback;                /* start each written chunk's writeback (one-pass layout) */
    pq_column* spare;              /* [nspare] each worker's buffers between its tasks */
    int64_t nspare;
} pq_direct_work;

/* Trade a task cursor's page, dictionary and symbol buffers with its
 * worker's spare ones: a worker grows them once for all its tasks, instead
 * of each task allocating and doubling its own.  Buffers past PQ_SPARE_MAX
 * (large pages or dictionaries of one column) are freed when the pass that
 * grew them ends, not held to the end of the import. */
#define PQ_SPARE_MAX ((size_t)4 << 20)
static void pq_trade_buffers(pq_column* c, pq_column* spare) {
    uint8_t* page = c->page; size_t page_cap = c->page_cap;
    uint8_t* dict = c->dict; size_t dict_cap = c->dict_cap;
    pq_symbol_scratch* symbols = c->symbols;
    c->page = spare->page; c->page_cap = spare->page_cap;
    c->dict = spare->dict; c->dict_cap = spare->dict_cap;
    c->symbols = spare->symbols;
    spare->page = page; spare->page_cap = page_cap;
    spare->dict = dict; spare->dict_cap = dict_cap;
    spare->symbols = symbols;
}

/* Byte range of every column chunk — from its dictionary page (or first
 * data page) for total_compressed bytes — and each column's uncompressed
 * total.  `col_vocab` (may be NULL): each column's distinct strings'
 * bytes, bounded from the footer alone: a chunk's dictionary page (its
 * stored bytes), or the whole chunk where it has none or holds more than
 * the dictionary's ids could take (at most 4 bytes and a level a value:
 * the writer fell back to PLAIN pages).  False when a row group's metadata
 * cannot be read. */
static bool pq_chunk_ranges(ray_parquet_t* r, int64_t* lo, int64_t* hi, int64_t* col_bytes, int64_t* col_vocab) {
    pq_span* cols = ray_calloc_raw((size_t)r->ncols*sizeof(*cols));
    bool ok = cols != NULL;
    for (int64_t g = 0; ok && g < r->ngroups; g++) {
        int64_t rows = 0;
        if (!pq_group_columns(r,g,cols,&rows,NULL)) { ok = false; break; }
        for (int64_t c = 0; c < r->ncols; c++) {
            int64_t i = g*r->ncols+c; pq_span mf[17];
            lo[i] = hi[i] = -1;
            if (!pq_fields(cols[c],mf,17)) continue;
            int64_t data = pq_get(mf[9],-1), dict = pq_get(mf[11],-1), bytes = pq_get(mf[7],-1);
            int64_t raw = pq_get(mf[6],-1), start = dict > 0 && dict < data ? dict : data;
            if (raw > 0) col_bytes[c] = raw > INT64_MAX - col_bytes[c] ? INT64_MAX : col_bytes[c] + raw;
            if (col_vocab && raw > 0) {
                int64_t values = pq_get(mf[5],-1), v = raw;
                if (dict > 0 && dict < data && values >= 0 && values < INT64_MAX/8 && raw - (data-dict) <= values*5 + 4096)
                    v = data-dict < raw ? data-dict : raw;
                col_vocab[c] = v > INT64_MAX - col_vocab[c] ? INT64_MAX : col_vocab[c] + v;
            }
            if (start < 0 || bytes < 0 || start > (int64_t)r->size || bytes > (int64_t)r->size - start) continue;
            lo[i] = start; hi[i] = start + bytes;
        }
    }
    ray_free_raw(cols);
    return ok;
}

/* A mapped column chunk is read on first touch, one fault at a time per
 * worker, so a worker has a single read in flight; on storage with a high
 * per-request latency (a network block device) that bounds the import.  A
 * task asks the kernel for its whole chunk and for the same column's chunks
 * of the next `prefetch` row groups (up to `ahead_bytes`), so the reads are
 * in flight together while it decodes.  RAY_PQ_PREFETCH sets the depth
 * (default 2, 0 turns it off). */
static void pq_prefetch_chunks(pq_direct_work* w, int64_t g, int64_t c) {
    ray_parquet_t* r = w->parent;
    int64_t ahead = 0;
    for (int64_t k = 0; k <= w->prefetch && g + k < r->ngroups; k++) {
        int64_t i = (g+k)*r->ncols+c, lo = w->chunk_lo[i], hi = w->chunk_hi[i];
        if (lo < 0) continue;
        if (k > 0 && (ahead += hi - lo) > w->ahead_bytes) break;
        uint8_t z = 0;
        if (!atomic_compare_exchange_strong_explicit(&w->prefetched[i],&z,1,
                memory_order_relaxed,memory_order_relaxed)) continue;
        ray_vm_advise_willneed(r->map + lo,(size_t)(hi-lo));
    }
}
static void pq_write_direct_group(void* ptr, uint32_t worker, int64_t start, int64_t end) {
    pq_direct_work* w = ptr;
    pq_column* spare = (int64_t)worker < w->nspare ? &w->spare[worker] : NULL;
    for (int64_t task = start; task < end; task++) {
        /* Row groups outermost within a pass: a worker reads one group's
         * chunks of the pass's columns side by side. */
        int64_t g = task/w->npass, c = w->pass[task%w->npass];
        if (w->prefetched) pq_prefetch_chunks(w,g,c);
        ray_parquet_t* r = pq_group_reader(w->parent,g,1);
        if (!r) { w->errors[task] = ray_error("oom",NULL); continue; }
        /* A task is one column chunk, so both groups and columns can occupy
         * workers. Narrow schemas still get one task per row group. */
        r->selected[0] = (int32_t)c; r->nselected = r->noutput = 1;
        if (spare) pq_trade_buffers(&r->cursors[0],spare);
        int64_t row = w->offsets[g];
        ray_col_stream_t local = {0};
        local.type = w->writers[c].type; local.dom = w->writers[c].dom;
        size_t size = local.type == RAY_SYM ? 4 : ray_elem_size(local.type);
        local.fp = fopen(w->writers[c].tmp_path,"r+b");
        if (!local.fp) {
            w->errors[task] = pq_error("cannot open native column");
            if (spare) pq_trade_buffers(&r->cursors[0],spare);
            ray_parquet_close(r); continue;
        }
        int64_t offset = 32+row*(int64_t)size;
#ifdef RAY_OS_WINDOWS
        bool seek = _fseeki64(local.fp,offset,SEEK_SET) == 0;
#else
        bool seek = fseeko(local.fp,(off_t)offset,SEEK_SET) == 0;
#endif
        if (!seek) w->errors[task] = pq_error("cannot seek native output range");
        if (!w->errors[task]) {
            ray_err_t ie = ray_col_stream_index_begin(&local,row);
            if (ie != RAY_OK) w->errors[task] = ray_error(ray_err_code_str(ie),"parquet: zone accumulator");
        }
        while (!w->errors[task]) {
            ray_t* batch = ray_parquet_next(r);
            if (!batch) break;
            if (RAY_IS_ERR(batch)) { w->errors[task] = batch; break; }
            ray_err_t err = ray_col_stream_append(&local,ray_table_get_col_idx(batch,0));
            row += ray_table_nrows(batch); ray_release(batch);
            if (err != RAY_OK) { w->errors[task] = ray_error(ray_err_code_str(err),"parquet: direct column write failed"); break; }
        }
        /* Fold this row group's chunk partials into the column's zone. */
        if (local.zone && !w->errors[task]) {
            while (atomic_exchange_explicit(&w->locks[c],1,memory_order_acquire)) RAY_CPU_RELAX();
            ray_err_t me = ray_col_stream_index_merge(&w->writers[c],&local);
            atomic_store_explicit(&w->locks[c],0,memory_order_release);
            if (me != RAY_OK && !w->errors[task]) w->errors[task] = ray_error(ray_err_code_str(me),"parquet: zone merge");
        }
        /* Unmerged accumulator (task error): ray_col_stream_abort would
         * fclose local.fp a second time, so free the zone directly. */
        if (local.zone) { ray_zone_acc_free(local.zone); ray_free_raw(local.zone); local.zone = NULL; }
        /* Start this chunk's writeback now: the pages leave while the
         * import goes on, instead of all at the final sync.  A failed flush
         * has dropped its buffer, and the close after it may well succeed:
         * it is the task's error. */
        if (w->writeback && !w->errors[task]) {
            if (fflush(local.fp) != 0) w->errors[task] = pq_error("native column write failed");
            else ray_file_writeback_start(fileno(local.fp),32+w->offsets[g]*(int64_t)size,
                                          (row-w->offsets[g])*(int64_t)size);
        }
        if (fclose(local.fp) && !w->errors[task]) w->errors[task] = pq_error("native column close failed");
        /* The task's writer owns no file of its own (tmp_path is the
         * column's, left unset here): abort only frees what an append may
         * have made, such as the runtime id cache of a runtime-domain
         * chunk. */
        local.fp = NULL;
        ray_col_stream_abort(&local);
        if (local.had_nulls) atomic_store_explicit(&w->nulls[c],1,memory_order_relaxed);
        if (!w->errors[task] && row != w->offsets[g+1]) w->errors[task] = pq_error("row group ended before its assigned output range");
        if (spare) pq_trade_buffers(&r->cursors[0],spare);
        ray_parquet_close(r);
    }
}

/* While the passes run, a background thread appends the file domain's new
 * entries to the symbol file every quarter second (and starts their
 * writeback), so the final flush writes only the tail and its sync finds
 * the rest on disk. */
typedef struct { ray_sym_domain_t* dom; _Atomic(bool) stop; _Atomic(int) err; } pq_symflush_t;
static void pq_symflush_fn(void* arg) {
    pq_symflush_t* s = arg;
    while (!atomic_load_explicit(&s->stop,memory_order_acquire)) {
        ray_err_t e = ray_sym_domain_flush_append(s->dom,false);
        if (e != RAY_OK) { atomic_store_explicit(&s->err,(int)e,memory_order_release); return; }
        /* in slices, so the import's end does not wait out the interval */
        for (int i = 0; i < 25 && !atomic_load_explicit(&s->stop,memory_order_acquire); i++)
            ray_sleep_ms(10);
    }
}

/* ---- the grouped symbol import (table/symgrp.h) ---------------------------
 * A symbol column whose strings would make the import dictionary too big to
 * probe at random goes in two decodes of its chunks: the first deduplicates
 * each chunk and stages its distinct strings' hashes and lengths, the
 * dictionary then gives them verdicts one hash group at a time, and the
 * second decode writes the new strings, compares every candidate's bytes
 * with its record and writes the codes.  No string is kept between the two;
 * the symbol file is only appended to and read in position order. */
typedef enum { PQ_SYM_AUTO, PQ_SYM_DIRECT, PQ_SYM_GROUPED } pq_sym_mode;

/* One chunk's exact dedupe: local ids by first occurrence (0 is the null
 * and "" row), a repeat found by length, then hash, then bytes.  The bytes
 * of the generation's strings sit in an arena; when it is full the table
 * starts over, so a later repeat gets an id of its own, which the
 * dictionary links to the first.  Both decodes make the same ids. */
typedef struct {
    ray_symgrp_fp_t* fp;        /* local id i + 1 at fp[i] */
    uint32_t* cnt;              /* FREQ: rows per local id */
    int64_t n, cap;
    uint32_t* slot; uint64_t mask;
    int64_t* aoff;              /* arena offset, by local id - first */
    char* arena; int64_t abytes, acap, first, gens;
    int64_t alim;               /* a generation's arena bytes (the pass's; the
                                 * arena may have grown past it for one string) */
} pq_dedup;

static void pq_dedup_free(pq_dedup* d) {
    ray_free_raw(d->fp); ray_free_raw(d->cnt); ray_free_raw(d->slot);
    ray_free_raw(d->aoff); ray_free_raw(d->arena);
    memset(d,0,sizeof(*d));
}
/* The arena starts small and doubles up to a generation's bytes: a worker
 * whose chunks are small never holds the generation limit's memory. */
#define PQ_DEDUP_ARENA0 ((int64_t)64 << 10)
static bool pq_dedup_init(pq_dedup* d, int64_t alim, bool counts) {
    memset(d,0,sizeof(*d));
    d->alim = alim; d->acap = alim < PQ_DEDUP_ARENA0 ? alim : PQ_DEDUP_ARENA0;
    d->first = 1; d->cap = 1024; d->mask = 2047;
    d->fp = ray_alloc_raw((size_t)d->cap*sizeof(*d->fp));
    d->aoff = ray_alloc_raw((size_t)d->cap*sizeof(*d->aoff));
    d->slot = ray_calloc_raw((size_t)(d->mask+1)*sizeof(*d->slot));
    d->arena = ray_alloc_raw((size_t)(d->acap > 0 ? d->acap : 1));
    if (counts) d->cnt = ray_calloc_raw((size_t)(d->cap+1)*sizeof(*d->cnt));
    return d->fp && d->aoff && d->slot && d->arena && (!counts || d->cnt);
}
/* A worker's dedupe for its next chunk, the buffers its earlier chunks grew
 * kept (warm pages, no allocation a task).  The ids depend only on the
 * chunk and the generation limit, so both decodes still agree. */
static bool pq_dedup_reset(pq_dedup* d, int64_t alim, bool counts) {
    if (!d->fp) return pq_dedup_init(d,alim,counts);
    d->n = 0; d->first = 1; d->abytes = 0; d->gens = 0; d->alim = alim;
    memset(d->slot,0,(size_t)(d->mask+1)*sizeof(*d->slot));
    if (counts && !d->cnt) d->cnt = ray_alloc_raw((size_t)(d->cap+1)*sizeof(*d->cnt));
    if (counts && !d->cnt) return false;
    if (d->cnt) memset(d->cnt,0,(size_t)(d->cap+1)*sizeof(*d->cnt));
    return true;
}
static bool pq_dedup_rehash(pq_dedup* d, uint64_t cap) {
    uint32_t* s = ray_calloc_raw((size_t)cap*sizeof(*s));
    if (!s) return false;
    ray_free_raw(d->slot); d->slot = s; d->mask = cap-1;
    for (int64_t id = d->first; id <= d->n; id++) {
        uint64_t i = d->fp[id-1].h & d->mask;
        while (d->slot[i]) i = (i+1) & d->mask;
        d->slot[i] = (uint32_t)id;
    }
    return true;
}
/* The local id of a non-empty string; *fresh when it is its first
 * occurrence (of the generation).  0 on allocation failure. */
static uint32_t pq_dedup_id(pq_dedup* d, uint64_t h, const char* s, uint32_t len, bool* fresh) {
    uint64_t i = h & d->mask;
    for (uint32_t v; (v = d->slot[i]); i = (i+1) & d->mask) {
        const ray_symgrp_fp_t* f = &d->fp[v-1];
        if (f->len == len && f->h == h && !memcmp(d->arena+d->aoff[v-d->first],s,len)) { *fresh = false; return v; }
    }
    *fresh = true;
    if (d->n >= UINT32_MAX-1) return 0;
    if (d->abytes+len > d->alim && d->n >= d->first) {
        /* a new generation */
        memset(d->slot,0,(size_t)(d->mask+1)*sizeof(*d->slot));
        d->abytes = 0; d->first = d->n+1; d->gens++;
        i = h & d->mask;
    }
    if (d->n == d->cap) {
        int64_t nc = d->cap*2;
        ray_symgrp_fp_t* fp = ray_realloc_raw(d->fp,(size_t)nc*sizeof(*fp));
        if (!fp) return 0;
        d->fp = fp;
        if (d->cnt) {
            uint32_t* c = ray_realloc_raw(d->cnt,(size_t)(nc+1)*sizeof(*c));
            if (!c) return 0;
            memset(c+d->cap+1,0,(size_t)(nc-d->cap)*sizeof(*c));
            d->cnt = c;
        }
        d->cap = nc;
    }
    if (d->n+1-d->first >= (int64_t)(d->mask+1)/2) {
        int64_t* ao = ray_realloc_raw(d->aoff,(size_t)(d->mask+1)*sizeof(*ao));
        if (!ao || (d->aoff = ao, !pq_dedup_rehash(d,(d->mask+1)*2))) return 0;
        i = h & d->mask;
        while (d->slot[i]) i = (i+1) & d->mask;
    }
    if (d->abytes+len > d->acap) {
        /* doubled up to the generation's bytes (past them by one string at
         * most) */
        int64_t need = d->abytes+len, nc = d->acap*2;
        if (nc > d->alim) nc = d->alim;
        if (nc < need) nc = need;
        char* a = ray_realloc_raw(d->arena,(size_t)nc);
        if (!a) return 0;
        d->arena = a; d->acap = nc;
    }
    if (len) memcpy(d->arena+d->abytes,s,len);
    d->aoff[d->n+1-d->first] = d->abytes; d->abytes += len;
    d->fp[d->n] = (ray_symgrp_fp_t){h,len,(uint32_t)(d->n+1)};
    d->slot[i] = (uint32_t)(++d->n);
    return (uint32_t)d->n;
}

/* A worker's buffers for the pass's chunks, kept from one chunk to the
 * next: grown once, their pages warm, freed with the pass. */
typedef struct {
    pq_dedup d;
    uint32_t* dmap; int64_t dcap;            /* dictionary index -> local id */
    ray_symgrp_res_t* res; uint32_t* map; int64_t rcap;
    ray_t* vec;                              /* a batch of codes */
    char* wbuf;                              /* records on their way to the file, 2 PQ_GWBUF */
} pq_gscratch;
#define PQ_GWBUF ((int64_t)1 << 20)

/* The trace's split of the second decode: verdicts, walk, finish (per
 * task, summed over the workers) and the windows' load and settle. */
enum { PQ_GS_VERDICTS, PQ_GS_WALK, PQ_GS_FINISH, PQ_GS_LOAD, PQ_GS_SETTLE, PQ_GS_N };
typedef struct { int64_t ns, majflt, rd, wr; } pq_tio_t;

typedef struct {
    pq_direct_work* dw;
    int64_t c;                   /* the column */
    ray_symgrp_t* g;
    ray_symimp_t* imp;
    int64_t ta, tb;              /* the window of the second decode */
    int64_t acap;                /* dedupe arena per task */
    uint64_t hmask;              /* the hashes kept (all but in a collision test) */
    bool counts, redo, trace;
    pq_gscratch* ws;             /* [nws], by worker */
    int64_t nws;
    _Atomic(int64_t) rows, gens;
    _Atomic(int64_t) split[PQ_GS_N][4];
} pq_gwork;

/* One chunk's walk, either decode. */
typedef struct {
    pq_gwork* w;
    int64_t t;
    uint32_t worker;
    pq_dedup d;
    uint32_t* dmap;              /* dictionary index -> local id */
    int64_t dcap;
    bool dmapped;
    bool second;
    int64_t nres;                /* second: local ids the first decode made */
    ray_symgrp_res_t* res;       /* second: verdicts by local id */
    uint32_t* map;               /* second: positions by local id */
    /* second, ROWS*: the task's records in two runs, [1] those later tasks
     * meet again, [0] the others (ray_symgrp_task_off) */
    int64_t cursor[2];           /* the next record's offset */
    char* wbuf[2];               /* records not yet written, */
    int64_t wlen[2], woff[2];    /*   wlen bytes for file offset woff on */
    uint32_t* codes; int64_t ncodes;
    ray_t* vec; ray_col_stream_t* out;
    int64_t cmp, cmpb;
    const char* err;
} pq_gtask;

static bool pq_g_inject(const char* step);
/* Records to the symbol file (RAY_PQ_SYM_INJECT=write fails them). */
static bool pq_g_write(pq_gtask* k, int64_t off, const void* p, size_t n) {
    return !pq_g_inject("write") && ray_symimp_write(k->w->imp,off,p,n);
}
/* The task's buffered records of run `a` to the file. */
static bool pq_g_wflush1(pq_gtask* k, int a) {
    if (!k->wlen[a]) return true;
    bool ok = pq_g_write(k,k->woff[a],k->wbuf[a],(size_t)k->wlen[a]);
    k->woff[a] += k->wlen[a]; k->wlen[a] = 0;
    return ok;
}
static bool pq_g_wflush(pq_gtask* k) { return pq_g_wflush1(k,0) && pq_g_wflush1(k,1); }
/* ROWS*: record `pos` at `off`, run a's next: through the run's buffer,
 * written with pwrite in large requests (the mapping's pages are never
 * faulted in to be overwritten). */
static bool pq_g_record(pq_gtask* k, int a, int64_t pos, int64_t off, const char* s, uint32_t len) {
    ray_symimp_place(k->w->imp,pos,off);
    if (k->wlen[a] && k->woff[a] + k->wlen[a] != off && !pq_g_wflush1(k,a)) return false;
    if (k->wlen[a] + 4 + (int64_t)len > PQ_GWBUF && !pq_g_wflush1(k,a)) return false;
    if (!k->wlen[a]) k->woff[a] = off;
    if (4 + (int64_t)len > PQ_GWBUF)   /* past the buffer: straight to the file */
        return pq_g_write(k,off,&len,4) && pq_g_write(k,off+4,s,len) &&
               (k->woff[a] = off + 4 + len, true);
    memcpy(k->wbuf[a] + k->wlen[a],&len,4);
    if (len) memcpy(k->wbuf[a] + k->wlen[a] + 4,s,len);
    k->wlen[a] += 4 + (int64_t)len;
    return true;
}
/* Whether `s` is this task's own new record at `pos` (maybe still in one
 * of its buffers). */
static bool pq_g_same_own(pq_gtask* k, uint32_t pos, const char* s, uint32_t len) {
    int64_t off = ray_symimp_offset(k->w->imp,pos);
    for (int a = 0; a < 2; a++)
        if (k->wlen[a] && off >= k->woff[a] && off < k->woff[a] + k->wlen[a]) {
            const char* r = k->wbuf[a] + (off - k->woff[a]);
            uint32_t rl; memcpy(&rl,r,4);
            return rl == len && (!len || !memcmp(r + 4,s,len));
        }
    return ray_symgrp_same(k->w->g,pos,s,len);
}

/* A local id's first occurrence in the second decode: its verdict. */
static bool pq_g2_first(pq_gtask* k, uint32_t local, uint64_t h, const char* s, uint32_t len) {
    ray_symgrp_res_t* v = &k->res[local];
    int64_t pos = v->pos;
    if (v->kind == RAY_SYMGRP_NEW) {
        /* a redo wrote it already */
        if (v->off < 0) {
            int a = v->again ? 1 : 0;
            int64_t off = k->cursor[a]; k->cursor[a] += 4+(int64_t)len;
            if (!k->w->redo && !pq_g_record(k,a,pos,off,s,len)) return false;
        } else if (!k->w->redo) ray_symimp_put(k->w->imp,pos,v->off,s,len);
    } else if (v->kind == RAY_SYMGRP_OLD || v->kind == RAY_SYMGRP_REF) {
        if (k->w->redo) {
            /* compared in the first run: a mismatch has the position the
             * window's settle gave it */
            int64_t o = ray_symgrp_override(k->w->g,k->t,local);
            if (o >= 0) pos = o;
        } else if (v->kind == RAY_SYMGRP_REF && v->owner != k->t && v->owner >= k->w->ta) {
            /* another task of the window writes the record: compared after it */
            if (!ray_symgrp_defer(k->w->g,k->worker,k->t,local,h,(uint32_t)pos,s,len)) return false;
        } else {
            /* compared now; a mismatch takes its position at the window's
             * end, in an order the workers do not decide (the codes written
             * meanwhile are written again) */
            k->cmp++; k->cmpb += len;
            bool same = v->kind == RAY_SYMGRP_REF && v->owner == k->t ? pq_g_same_own(k,(uint32_t)pos,s,len)
                                                                      : ray_symgrp_same(k->w->g,(uint32_t)pos,s,len);
            if (!same && !ray_symgrp_mismatch(k->w->g,k->worker,k->t,local,h,s,len)) return false;
        }
    } else return false;
    k->map[local] = (uint32_t)pos;
    return true;
}

/* A string's local id, first occurrences seen to. */
static bool pq_g_local(pq_gtask* k, const char* s, uint32_t len, uint32_t* local) {
    if (!len) { *local = 0; return true; }
    uint64_t h = ray_hash_bytes(s,len) & k->w->hmask;
    bool fresh;
    uint32_t id = pq_dedup_id(&k->d,h,s,len,&fresh);
    if (!id || (k->second && id > k->nres)) return false;
    if (fresh && k->second && !pq_g2_first(k,id,h,s,len)) return false;
    *local = id;
    return true;
}

static bool pq_g_flush(pq_gtask* k) {
    if (!k->ncodes) return true;
    k->vec->len = k->ncodes;
    ray_err_t e = ray_col_stream_append(k->out,k->vec);
    k->ncodes = 0;
    return e == RAY_OK;
}
/* `run` rows of local id `local`. */
static bool pq_g_emit(pq_gtask* k, uint32_t local, int64_t run) {
    if (k->d.cnt) k->d.cnt[local] += (uint32_t)run;
    if (!k->second) return true;
    uint32_t pos = k->map[local];
    while (run > 0) {
        int64_t room = PQ_BATCH-k->ncodes, m = run < room ? run : room;
        for (int64_t i = 0; i < m; i++) k->codes[k->ncodes+i] = pos;
        k->ncodes += m; run -= m;
        if (k->ncodes == PQ_BATCH && !pq_g_flush(k)) return false;
    }
    return true;
}

/* The chunk's rows, as pq_decode_symbols reads them. */
static const char* pq_g_walk(pq_column* c, pq_schema* s, int64_t rows, pq_gtask* k) {
    for (int64_t at = 0; at < rows;) {
        if (ray_interrupted()) return "scan interrupted";
        if (!c->page_left) { const char* err = pq_page(c,s,NULL); if (err) return err; }
        if (c->have_dict && !k->dmapped) {
            if (k->dcap < c->dict_count) {
                uint32_t* m = ray_realloc_raw(k->dmap,(size_t)c->dict_count*sizeof(*k->dmap));
                if (!m) return "symbol dictionary allocation failed";
                k->dmap = m; k->dcap = c->dict_count;
            }
            k->dmapped = true;
            for (int64_t i = 0; i < c->dict_count; i++)
                if (!pq_g_local(k,(const char*)c->strings[i].p,c->strings[i].n,&k->dmap[i])) return "symbol allocation failed";
        }
        int64_t n = rows-at < c->page_left ? rows-at : c->page_left;
        for (int64_t i = 0; i < n; i++) {
            uint32_t local;
            if (!c->optional && c->encoding && c->ids.left && !c->ids.packed) {
                uint32_t id = c->ids.value;
                if (id >= c->dict_count || !k->dmapped) return "invalid symbol dictionary index";
                local = k->dmap[id];
                if (!local && s->strict) return "nonnull empty text collides with native null";
                int64_t run = n-i < (int64_t)c->ids.left ? n-i : (int64_t)c->ids.left;
                if (!pq_g_emit(k,local,run)) return "symbol code write failed";
                c->ids.left -= (uint64_t)run; i += run-1; continue;
            }
            uint32_t present = 1;
            if (c->optional && (!pq_rle_next(&c->defs,&present) || present > 1)) return "invalid definition levels";
            if (!present) { c->page_nulls++; if (!pq_g_emit(k,0,1)) return "symbol code write failed"; continue; }
            if (c->encoding) {
                uint32_t id;
                if (!pq_rle_next(&c->ids,&id) || id >= c->dict_count || !k->dmapped) return "invalid symbol dictionary index";
                local = k->dmap[id];
                if (!local && s->strict) return "nonnull empty text collides with native null";
            } else {
                const uint8_t *lp, *p;
                if (!pq_take(&c->values,4,&lp)) return "truncated string length";
                uint32_t len = pq_u32(lp);
                if (!pq_take(&c->values,len,&p)) return "truncated string data";
                if (s->strict && (!len || (s->converted == 0 && !pq_utf8(p,len))))
                    return "nonnull text is empty or invalid UTF-8";
                if (!pq_g_local(k,(const char*)p,len,&local)) return "symbol allocation failed";
            }
            if (!pq_g_emit(k,local,1)) return "symbol code write failed";
        }
        at += n; c->page_left -= n; c->chunk_left -= n;
        if (!c->page_left) {
            if (c->optional && !pq_rle_done(&c->defs)) return "excess definition levels";
            if (c->expected_nulls >= 0 && c->page_nulls != c->expected_nulls) return "v2 null count mismatch";
            if (c->encoding) { if (!pq_rle_done(&c->ids)) return "excess dictionary indices"; }
            else if (c->values.p != c->values.end) return "excess string values";
            if (!c->chunk_left && c->chunk.p != c->chunk.end) return "excess column pages";
        }
    }
    return NULL;
}

/* Fault injection for the suite: RAY_PQ_SYM_INJECT names the step that
 * fails as out of memory ("r1", "p2", "load", "r2", "settle"; "reload" in
 * table/symgrp.c: a window's load after the first), whose record writes
 * fail ("write"), or is cancelled ("cancel"); debug builds only. */
static bool pq_g_inject(const char* step) {
#if defined(DEBUG)
    const char* e = getenv("RAY_PQ_SYM_INJECT");
    if (!e || strcmp(e,step) != 0) return false;
    if (!strcmp(step,"cancel")) { ray_request_interrupt(); return false; }
    return true;
#else
    (void)step; return false;
#endif
}

/* A grouped task's error: "cancel" once the import is interrupted (the
 * walks stop on it with an error of their own), as the direct import has it. */
static ray_t* pq_g_error(const char* err) {
    return ray_interrupted() ? ray_error("cancel","parquet conversion interrupted") : pq_error(err);
}

/* The chunk's reader, its cursor holding the worker's buffers. */
static ray_parquet_t* pq_g_reader(pq_gwork* w, int64_t g, uint32_t worker, const char** err) {
    pq_direct_work* dw = w->dw;
    ray_parquet_t* r = pq_group_reader(dw->parent,g,1);
    if (!r) { *err = "row group allocation failed"; return NULL; }
    r->selected[0] = (int32_t)w->c; r->nselected = r->noutput = 1;
    if ((int64_t)worker < dw->nspare) pq_trade_buffers(&r->cursors[0],&dw->spare[worker]);
    r->group++;
    *err = pq_start_group(r);
    return r;
}
static void pq_g_reader_close(pq_gwork* w, ray_parquet_t* r, uint32_t worker) {
    if ((int64_t)worker < w->dw->nspare) pq_trade_buffers(&r->cursors[0],&w->dw->spare[worker]);
    ray_parquet_close(r);
}
/* The chunk read ahead again for the second decode (and the same column's
 * next chunks), whatever the first asked for. */
static void pq_g_prefetch(pq_direct_work* w, int64_t g, int64_t c) {
    if (!w->prefetched) return;
    ray_parquet_t* r = w->parent;
    int64_t ahead = 0;
    for (int64_t k = 0; k <= w->prefetch && g + k < r->ngroups; k++) {
        int64_t i = (g+k)*r->ncols+c, lo = w->chunk_lo[i], hi = w->chunk_hi[i];
        if (lo < 0) continue;
        if (k > 0 && (ahead += hi - lo) > w->ahead_bytes) break;
        ray_vm_advise_willneed(r->map + lo,(size_t)(hi-lo));
    }
}

/* The calling thread's major faults, read and written bytes and time
 * (the trace's split; zero without it). */
#if defined(__linux__) && !defined(RUSAGE_THREAD)
#define RUSAGE_THREAD 1
#endif
static pq_tio_t pq_tio_now(bool on) {
    pq_tio_t io = {0};
    if (!on) return io;
    io.ns = ray_profile_now_ns();
#if defined(__linux__)
    struct rusage ru;
    if (getrusage(RUSAGE_THREAD,&ru) == 0) io.majflt = ru.ru_majflt;
    FILE* f = fopen("/proc/thread-self/io","r");
    if (f) {
        char line[128];
        while (fgets(line,sizeof(line),f)) {
            long long v;
            if (sscanf(line,"read_bytes: %lld",&v) == 1) io.rd = v;
            else if (sscanf(line,"write_bytes: %lld",&v) == 1) io.wr = v;
        }
        fclose(f);
    }
#endif
    return io;
}
static void pq_tio_add(pq_gwork* w, int part, pq_tio_t a, pq_tio_t b) {
    if (!w->trace) return;
    atomic_fetch_add_explicit(&w->split[part][0],b.ns-a.ns,memory_order_relaxed);
    atomic_fetch_add_explicit(&w->split[part][1],b.majflt-a.majflt,memory_order_relaxed);
    atomic_fetch_add_explicit(&w->split[part][2],b.rd-a.rd,memory_order_relaxed);
    atomic_fetch_add_explicit(&w->split[part][3],b.wr-a.wr,memory_order_relaxed);
}

/* The worker's buffers into the task, and back after it. */
static pq_gscratch* pq_g_scratch(pq_gwork* w, uint32_t worker, pq_gtask* k) {
    pq_gscratch* ws = (int64_t)worker < w->nws ? &w->ws[worker] : NULL;
    if (ws) { k->d = ws->d; k->dmap = ws->dmap; k->dcap = ws->dcap; }
    return ws;
}
static void pq_g_scratch_back(pq_gscratch* ws, pq_gtask* k) {
    if (ws) { ws->d = k->d; ws->dmap = k->dmap; ws->dcap = k->dcap; }
    else { pq_dedup_free(&k->d); ray_free_raw(k->dmap); }
}

/* First decode of row group g: stage its distinct strings. */
static void pq_g1_task(void* ptr, uint32_t worker, int64_t start, int64_t end) {
    pq_gwork* w = ptr; pq_direct_work* dw = w->dw;
    for (int64_t g = start; g < end; g++) {
        if (dw->prefetched) pq_prefetch_chunks(dw,g,w->c);
        pq_gtask k = {.w = w, .t = g, .worker = worker};
        pq_gscratch* ws = pq_g_scratch(w,worker,&k);
        const char* err = NULL;
        ray_parquet_t* r = pq_g_reader(w,g,worker,&err);
        if (!err && !pq_dedup_reset(&k.d,w->acap,w->counts)) err = "symbol dedupe allocation failed";
        if (!err && pq_g_inject("r1")) err = "symbol dedupe allocation failed";
        if (!err && r->group_left) {
            pq_schema schema = r->schema[w->c]; schema.import_domain = NULL;
            err = pq_g_walk(&r->cursors[0],&schema,r->group_left,&k);
        }
        if (!err) atomic_fetch_add_explicit(&w->rows,r->group_rows,memory_order_relaxed);
        if (!err && !ray_symgrp_stage(w->g,g,k.d.n,k.d.fp,k.d.cnt ? k.d.cnt+1 : NULL))
            err = "symbol staging allocation failed";
        atomic_fetch_add_explicit(&w->gens,k.d.gens,memory_order_relaxed);
        if (err && !dw->errors[g]) dw->errors[g] = pq_g_error(err);
        pq_g_scratch_back(ws,&k);
        if (r) pq_g_reader_close(w,r,worker);
    }
}

/* Second decode of row group g: records, comparisons, codes. */
static void pq_g2_task(void* ptr, uint32_t worker, int64_t start, int64_t end) {
    pq_gwork* w = ptr; pq_direct_work* dw = w->dw;
    for (int64_t i = start; i < end; i++) {
        int64_t g = w->redo ? i : w->ta+i;   /* a redo is given its task itself */
        pq_tio_t io0 = pq_tio_now(w->trace), io1 = io0, io2 = io0;
        pq_g_prefetch(dw,g,w->c);
        pq_gtask k = {.w = w, .t = g, .worker = worker, .second = true};
        pq_gscratch* ws = pq_g_scratch(w,worker,&k);
        ray_col_stream_t local = {0};
        const char* err = NULL;
        int64_t n = ray_symgrp_task_size(w->g,g);
        k.nres = n;
        ray_parquet_t* r = pq_g_reader(w,g,worker,&err);
        if (ws && ws->rcap < n+1) {
            ray_symgrp_res_t* res = ray_realloc_raw(ws->res,(size_t)(n+1)*sizeof(*res));
            if (res) ws->res = res;
            uint32_t* map = res ? ray_realloc_raw(ws->map,(size_t)(n+1)*sizeof(*map)) : NULL;
            if (map) { ws->map = map; ws->rcap = n+1; }
        }
        if (ws && !ws->vec) {
            ray_t* v = ray_sym_vec_new(RAY_SYM_W32,PQ_BATCH);
            if (v && !RAY_IS_ERR(v)) { v->sym_domain = dw->writers[w->c].dom; ray_sym_domain_retain(v->sym_domain); ws->vec = v; }
            else if (v) ray_release(v);
        }
        if (ws && !ws->wbuf) ws->wbuf = ray_alloc_raw((size_t)PQ_GWBUF*2);
        if (ws && ws->rcap >= n+1) { k.res = ws->res; k.map = ws->map; }
        if (ws) { k.vec = ws->vec; k.wbuf[0] = ws->wbuf; k.wbuf[1] = ws->wbuf ? ws->wbuf + PQ_GWBUF : NULL; }
        if (!err && (!ws || !k.res || !k.map || !k.vec || !k.wbuf[0] || !pq_dedup_reset(&k.d,w->acap,false)))
            err = "symbol code allocation failed";
        if (!err && pq_g_inject("r2")) err = "symbol code allocation failed";
        if (!err) (void)pq_g_inject("cancel");   /* the walk below sees it */
        if (!err && !ray_symgrp_task(w->g,g,k.res)) err = "symbol verdicts unavailable";
        io1 = pq_tio_now(w->trace);
        if (!err) {
            k.map[0] = 0;
            k.cursor[0] = ray_symgrp_task_off(w->g,g,false);
            k.cursor[1] = ray_symgrp_task_off(w->g,g,true);
            k.codes = (uint32_t*)ray_data(k.vec);
            k.ncodes = 0;
            local.type = RAY_SYM; local.dom = dw->writers[w->c].dom;
            local.fp = fopen(dw->writers[w->c].tmp_path,"r+b");
            if (!local.fp) err = "cannot open native column";
        }
        if (!err) {
            int64_t offset = 32+dw->offsets[g]*4;
#ifdef RAY_OS_WINDOWS
            bool seek = _fseeki64(local.fp,offset,SEEK_SET) == 0;
#else
            bool seek = fseeko(local.fp,(off_t)offset,SEEK_SET) == 0;
#endif
            if (!seek) err = "cannot seek native output range";
        }
        k.out = &local;
        if (!err && r->group_left) {
            pq_schema schema = r->schema[w->c]; schema.import_domain = NULL;
            err = pq_g_walk(&r->cursors[0],&schema,r->group_left,&k);
        }
        if (!err && !pq_g_wflush(&k)) err = "symbol file write failed";
        io2 = pq_tio_now(w->trace);
        if (!err && !pq_g_flush(&k)) err = "native column write failed";
        if (!err && dw->writeback) {
            if (fflush(local.fp) != 0) err = "native column write failed";
            else ray_file_writeback_start(fileno(local.fp),32+dw->offsets[g]*4,(dw->offsets[g+1]-dw->offsets[g])*4);
        }
        if (local.fp && fclose(local.fp) && !err) err = "native column close failed";
        local.fp = NULL;
        ray_col_stream_abort(&local);   /* frees what an append made; no file of its own */
        if (!err && local.rows != dw->offsets[g+1]-dw->offsets[g]) err = "row group ended before its assigned output range";
        if (local.had_nulls) atomic_store_explicit(&dw->nulls[w->c],1,memory_order_relaxed);
        if (err && !dw->errors[g]) dw->errors[g] = pq_g_error(err);
        ray_symgrp_note(w->g,k.cmp,k.cmpb);
        pq_g_scratch_back(ws,&k);
        if (r) pq_g_reader_close(w,r,worker);
        if (w->trace) {
            pq_tio_t io3 = pq_tio_now(true);
            if (!io2.ns) io2 = io3;
            pq_tio_add(w,PQ_GS_VERDICTS,io0,io1);
            pq_tio_add(w,PQ_GS_WALK,io1,io2);
            pq_tio_add(w,PQ_GS_FINISH,io2,io3);
        }
    }
}

/* /proc/self/io and the major faults, for the trace's phase lines. */
typedef struct { int64_t rd, wr, syscr, syscw, majflt; } pq_io_t;
static pq_io_t pq_io_now(void) {
    pq_io_t io = {0};
#if defined(__linux__)
    FILE* f = fopen("/proc/self/io","r");
    if (f) {
        char line[128];
        while (fgets(line,sizeof(line),f)) {
            long long v;
            if (sscanf(line,"read_bytes: %lld",&v) == 1) io.rd = v;
            else if (sscanf(line,"write_bytes: %lld",&v) == 1) io.wr = v;
            else if (sscanf(line,"syscr: %lld",&v) == 1) io.syscr = v;
            else if (sscanf(line,"syscw: %lld",&v) == 1) io.syscw = v;
        }
        fclose(f);
    }
    struct rusage ru;
    if (getrusage(RUSAGE_SELF,&ru) == 0) io.majflt = ru.ru_majflt;
#endif
    return io;
}
static void pq_io_print(const char* phase, pq_io_t a, pq_io_t b, int64_t t0, int64_t t1) {
    fprintf(stderr," %s=%.1fms rd=%.1fMB wr=%.1fMB syscr=%lld syscw=%lld majflt=%lld",phase,
            (double)(t1-t0)/1e6,(double)(b.rd-a.rd)/1048576.0,(double)(b.wr-a.wr)/1048576.0,
            (long long)(b.syscr-a.syscr),(long long)(b.syscw-a.syscw),(long long)(b.majflt-a.majflt));
}

/* One symbol column, grouped: errors land in dw->errors by row group. */
static void pq_sym_grouped(pq_direct_work* dw, ray_symgrp_t* g, ray_symimp_t* imp, int64_t c,
                           int groups, int64_t budget, int64_t workers, bool trace) {
    ray_parquet_t* r = dw->parent;
    ray_pool_t* pool = ray_pool_get();
    pq_gwork w = {.dw = dw, .c = c, .g = g, .imp = imp, .hmask = UINT64_MAX};
    w.acap = budget/4/workers;
    if (w.acap < ((int64_t)4 << 20)) w.acap = (int64_t)4 << 20;
    w.counts = ray_symgrp_order(g) == RAY_SYMGRP_FREQ;
#if defined(DEBUG)
    {
        const char* e = getenv("RAY_PQ_SYM_HASH_BITS");
        if (e && *e) { long b = strtol(e,NULL,10); w.hmask = b <= 0 ? 0 : b >= 64 ? UINT64_MAX : (UINT64_C(1) << b) - 1; }
        e = getenv("RAY_PQ_SYM_ARENA");
        if (e && *e && strtol(e,NULL,10) > 0) w.acap = strtol(e,NULL,10);
    }
#endif
    atomic_init(&w.rows,0); atomic_init(&w.gens,0);
    w.trace = trace;
    for (int p = 0; p < PQ_GS_N; p++) for (int q = 0; q < 4; q++) atomic_init(&w.split[p][q],0);
    int64_t tasks = r->ngroups;
    pq_io_t io0 = {0}, io1 = {0}, io2 = {0}, io3 = {0};
    int64_t t0 = 0, t1 = 0, t2 = 0, t3 = 0;
    if (trace) { io0 = pq_io_now(); t0 = ray_profile_now_ns(); }
    w.ws = (pq_gscratch*)ray_sys_alloc((size_t)workers*sizeof(pq_gscratch));   /* zero-filled */
    w.nws = w.ws ? workers : 0;
    /* the strings earlier direct passes added, into the index first */
    if (!w.ws || !ray_symgrp_adopt(g,w.hmask) || !ray_symgrp_begin(g,tasks,groups,workers)) { dw->errors[0] = ray_error("oom",NULL); goto out; }
    /* first decode */
    if (tasks > 0) {
        if (ray_pool_par_dispatch_ok(pool,tasks,2)) ray_pool_dispatch_n(pool,pq_g1_task,&w,(uint32_t)tasks);
        else pq_g1_task(&w,0,0,tasks);
    }
    for (int64_t t = 0; t < tasks; t++) if (dw->errors[t]) goto out;
    if (ray_interrupted()) goto out;
    if (trace) { io1 = pq_io_now(); t1 = ray_profile_now_ns(); }
    /* the verdicts, group by group */
    if (pq_g_inject("p2") || !ray_symgrp_resolve(g)) { dw->errors[0] = pq_g_error("symbol dictionary allocation or file growth failed"); goto out; }
    if (trace) { io2 = pq_io_now(); t2 = ray_profile_now_ns(); }
    /* second decode, window by window: the candidates' records of a window
     * (from earlier passes and windows) in half the budget */
    int64_t store = budget/2 < ((int64_t)16 << 20) ? (int64_t)16 << 20 : budget/2;
#if defined(DEBUG)
    {
        const char* e = getenv("RAY_PQ_SYM_WINDOW");
        if (e && *e && strtol(e,NULL,10) > 0) store = strtol(e,NULL,10);
    }
#endif
    for (int64_t ta = 0; ta < tasks;) {
        int64_t tb = ray_symgrp_window(g,ta,store);
        pq_tio_t la = pq_tio_now(trace);
        if (pq_g_inject("load") || !ray_symgrp_load(g,ta,tb)) { dw->errors[ta] = pq_g_error("symbol record load failed"); goto out; }
        pq_tio_add(&w,PQ_GS_LOAD,la,pq_tio_now(trace));
        w.ta = ta; w.tb = tb; w.redo = false;
        if (ray_pool_par_dispatch_ok(pool,tb-ta,2)) ray_pool_dispatch_n(pool,pq_g2_task,&w,(uint32_t)(tb-ta));
        else pq_g2_task(&w,0,0,tb-ta);
        for (int64_t t = ta; t < tb; t++) if (dw->errors[t]) goto out;
        if (ray_interrupted()) goto out;
        int64_t* redo = NULL; int64_t nredo = 0;
        pq_tio_t sa = pq_tio_now(trace);
        if (pq_g_inject("settle") || !ray_symgrp_settle(g,&redo,&nredo)) { dw->errors[ta] = pq_g_error("symbol comparison failed"); goto out; }
        pq_tio_add(&w,PQ_GS_SETTLE,sa,pq_tio_now(trace));
        /* the tasks whose deferred candidates did not match: their codes again */
        w.redo = true;
        for (int64_t i = 0; i < nredo && !dw->errors[redo[i]]; i++) pq_g2_task(&w,0,redo[i],redo[i]+1);
        ray_sys_free(redo);
        for (int64_t t = ta; t < tb; t++) if (dw->errors[t]) goto out;
        ray_symgrp_writeback(g,ta,tb);   /* the window's records start to disk */
        ta = tb;
    }
    /* the pass written: the header's count, and its records read at random
     * from now on (ray_symimp_sync) */
    (void)ray_symimp_sync(imp,false);
    if (trace) {
        io3 = pq_io_now(); t3 = ray_profile_now_ns();
        ray_symgrp_stats_t st;
        ray_symgrp_stats(g,&st);
        int64_t arena = 0;   /* the workers' dedupe arenas, as grown */
        for (int64_t i = 0; i < w.nws; i++) arena += w.ws[i].d.acap;
        ray_t* nm = ray_sym_str(r->schema[c].name);
        static const char* orders[] = {"rows","shards","freq","rowsflat"};
        fprintf(stderr,"parquet symgrp: col=%.*s order=%s groups=%d windows=%lld rows=%lld gens=%lld staged=%lld stage_mb=%.1f arena_mb=%.1f"
                " owners=%lld again=%lld again_mb=%.1f old=%lld refs=%lld log_loaded=%lld budget_mb=%.0f store_cap_mb=%.0f store_pos=%lld store_spans=%lld store_mb=%.1f"
                " store_read_mb=%.1f store_kept=%lld deferred=%lld settle_read_mb=%.1f compares=%lld cmp_mb=%.1f collisions=%lld redo=%lld entries=%lld rec_bytes=%lld wb_bytes=%lld adopted=%lld\n",
                (int)ray_str_len(nm),ray_str_ptr(nm),orders[ray_symgrp_order(g)],groups,(long long)st.windows,
                (long long)atomic_load(&w.rows),(long long)atomic_load(&w.gens),(long long)st.staged,(double)st.stage_bytes/1048576.0,(double)arena/1048576.0,
                (long long)st.owners,(long long)st.again,(double)st.again_bytes/1048576.0,(long long)st.old,(long long)st.refs,(long long)st.log_loaded,
                (double)budget/1048576.0,(double)store/1048576.0,
                (long long)st.store_pos,(long long)st.store_spans,(double)st.store_bytes/1048576.0,
                (double)st.store_read/1048576.0,(long long)st.store_kept,
                (long long)st.deferred,(double)st.settle_read/1048576.0,(long long)st.compares,(double)st.cmp_bytes/1048576.0,
                (long long)st.collisions,(long long)st.redo,(long long)ray_symgrp_entries(g),
                (long long)st.rec_bytes,(long long)st.wb_bytes,(long long)st.adopted);
        fprintf(stderr,"parquet symgrp io:");
        pq_io_print("decode1",io0,io1,t0,t1);
        pq_io_print("resolve",io1,io2,t1,t2);
        pq_io_print("decode2",io2,io3,t2,t3);
        fprintf(stderr,"\n");
        /* where decode2's time, faults and bytes go (the tasks' parts summed
         * over the workers; the windows' load and settle on this thread) */
        static const char* parts[PQ_GS_N] = {"verdicts","walk","finish","load","settle"};
        fprintf(stderr,"parquet symgrp split:");
        for (int p = 0; p < PQ_GS_N; p++)
            fprintf(stderr," %s=%.1fms/%lldflt/%.1fMBr/%.1fMBw",parts[p],
                    (double)atomic_load(&w.split[p][0])/1e6,(long long)atomic_load(&w.split[p][1]),
                    (double)atomic_load(&w.split[p][2])/1048576.0,(double)atomic_load(&w.split[p][3])/1048576.0);
        fprintf(stderr,"\n");
    }
out:
    for (int64_t i = 0; i < w.nws; i++) {
        pq_gscratch* s = &w.ws[i];
        pq_dedup_free(&s->d); ray_free_raw(s->dmap); ray_free_raw(s->res); ray_free_raw(s->map);
        ray_free_raw(s->wbuf);
        if (s->vec) ray_release(s->vec);
    }
    ray_sys_free(w.ws);
}

/* The import runs in passes, each decoding some columns' chunks from every
 * row group in parallel.  When memory is short next to the output it goes
 * column by column (a pass is one column, or a few when the file has fewer
 * row groups than the pool has workers): one column file is written at a
 * time instead of all of them at once.  Numeric and temporal columns go
 * first and are closed as soon as their pass ends, their hash index built
 * while the file is still in the page cache; symbol columns follow in
 * passes of their own, smallest first, so the file domain only grows once
 * the other columns are done.  With memory to spare one pass takes every
 * column, and the columns close and are indexed together after it, as the
 * row-group import does.  Symbol columns close with the domain, after its
 * flush. */
static ray_t* pq_write_direct(ray_parquet_t* r, ray_col_stream_t* writers) {
    int64_t chunks = r->ngroups*r->ncols;
    if (chunks > UINT32_MAX) return pq_error("too many column chunk tasks");
    int64_t* offsets = ray_calloc_raw((size_t)(r->ngroups+1)*sizeof(*offsets));
    _Atomic uint32_t* nulls = ray_calloc_raw((size_t)r->ncols*sizeof(*nulls));
    _Atomic uint32_t* locks = ray_calloc_raw((size_t)r->ncols*sizeof(*locks));
    ray_t** errors = ray_calloc_raw((size_t)(chunks+1)*sizeof(*errors));
    int64_t* lo = ray_alloc_raw((size_t)(chunks+1)*sizeof(*lo));
    int64_t* hi = ray_alloc_raw((size_t)(chunks+1)*sizeof(*hi));
    int64_t* bytes = ray_calloc_raw((size_t)r->ncols*sizeof(*bytes));
    int64_t* vocab = ray_calloc_raw((size_t)r->ncols*sizeof(*vocab));   /* pq_chunk_ranges */
    int64_t* order = ray_alloc_raw((size_t)r->ncols*sizeof(*order));
    _Atomic uint8_t* prefetched = NULL;
    pq_column* spare = NULL;   /* pq_direct_work.spare */
    int64_t nspare = 0;
    ray_symgrp_t* sgrp = NULL;   /* the grouped symbol import, or NULL */
    ray_symimp_t* simp = NULL;
    ray_t* err = NULL;
    pq_symflush_t symf = {NULL,false,(int)RAY_OK};
    ray_thread_t sym_thread = 0;
    bool sym_running = false;
    if (!offsets || !nulls || !locks || !errors || !lo || !hi || !bytes || !vocab || !order) { err = ray_error("oom",NULL); goto done; }
    for (int64_t c = 0; c < r->ncols; c++) {
        size_t size = writers[c].type == RAY_SYM ? 4 : ray_elem_size(writers[c].type);
        if (r->rows > (INT64_MAX-32)/(int64_t)size) { err = pq_error("native column exceeds file offset range"); goto done; }
        if (fflush(writers[c].fp)) { err = pq_error("cannot flush column header"); goto done; }
        atomic_init(&nulls[c],0); atomic_init(&locks[c],0);
        if (r->schema[c].native_symbol) r->schema[c].import_domain = writers[c].dom;
    }
    for (int64_t g = 0; g < r->ngroups; g++) {
        pq_span fields[8];
        if (!pq_fields(r->groups[g],fields,8)) { err = pq_error("invalid row group"); goto done; }
        offsets[g+1] = offsets[g]+pq_get(fields[3],0);
    }
    bool ranges = pq_chunk_ranges(r,lo,hi,bytes,vocab);
    /* numeric columns in schema order, then symbol columns by size */
    int64_t nnum = 0, n = 0;
    for (int64_t c = 0; c < r->ncols; c++) if (writers[c].type != RAY_SYM) order[n++] = c;
    nnum = n;
    for (int64_t c = 0; c < r->ncols; c++) if (writers[c].type == RAY_SYM) {
        int64_t j = n++;
        for (; j > nnum && bytes[order[j-1]] > bytes[c]; j--) order[j] = order[j-1];
        order[j] = c;
    }
    ray_pool_t* pool = ray_pool_get();
    int64_t workers = pool ? (int64_t)ray_pool_total_workers(pool) : 1;
    /* The pass layout follows memory: physical RAM or the cgroup limit,
     * the page cache the output competes for (not the heap budget of -m).
     * Column by column pays off when the output does not fit it with room
     * to spare: each column is written and indexed while it is hot, and
     * only it is.  With plenty of memory one pass over every column keeps
     * all the decode work overlapped with the symbol domain's commits and
     * wins.  RAY_PQ_PASS_COLS forces columns per pass, symbol passes
     * included (r->ncols: one pass). */
    int64_t out_bytes = 0;
    for (int64_t c = 0; c < r->ncols; c++) {
        int64_t b = r->rows*(int64_t)(writers[c].type == RAY_SYM ? 4 : ray_elem_size(writers[c].type));
        out_bytes = b > INT64_MAX - out_bytes ? INT64_MAX : out_bytes + b;
    }
    int64_t ram = ray_sys_ram_limit();
    bool colwise = ram > 0 && ram / PQ_COLWISE_RATIO < out_bytes;
    /* column by column: enough tasks in a pass for the pool, so a file of
     * few row groups still takes several columns at once */
    int64_t width = !colwise ? r->ncols
                  : r->ngroups > 0 ? (2*workers + r->ngroups - 1)/r->ngroups : r->ncols;
    if (width < 1) width = 1;
    bool forced = false;
    {
        const char* e = getenv("RAY_PQ_PASS_COLS");
        if (e && *e && strtol(e,NULL,10) > 0) { width = strtol(e,NULL,10); colwise = width < r->ncols; forced = true; }
    }
    if (width > r->ncols) width = r->ncols;
    bool trace = getenv("RAY_CSV_TRACE") != NULL;   /* the converters' phase trace */
    spare = ray_calloc_raw((size_t)workers*sizeof(*spare));   /* best effort */
    if (spare) nspare = workers;
    pq_direct_work work = {r,writers,offsets,nulls,locks,errors,NULL,0,lo,hi,NULL,0,0,true,spare,nspare};
    /* Starting each chunk's writeback as it is written spares one pass its
     * final sync; column by column the passes end their files anyway and
     * the extra I/O requests only slow the decode. */
    work.writeback = !colwise;
    {
        const char* e = getenv("RAY_PQ_PREFETCH");
        work.prefetch = (e && *e) ? strtol(e,NULL,10) : 2;
        if (work.prefetch > 0 && ranges) {
            prefetched = ray_calloc_raw((size_t)chunks+1);
            if (prefetched) for (int64_t i = 0; i < chunks; i++) atomic_init(&prefetched[i],0);
            work.prefetched = prefetched;
        }
        /* what the workers keep requested ahead together stays a small
         * share of memory */
        int64_t budget = ray_heap_anon_watermark()/16/workers;
        work.ahead_bytes = budget < ((int64_t)1 << 20) ? (int64_t)1 << 20
                         : budget > ((int64_t)64 << 20) ? (int64_t)64 << 20 : budget;
    }
    /* Symbol columns: one by one, smallest first, while memory is short
     * (the domain they share grows and is probed at random, and a column
     * at a time keeps the rest of the working set small); together in one
     * pass from half the output's bytes of memory, where their decode then
     * overlaps. */
    int64_t sym_width = forced ? width : ram > 0 && ram >= out_bytes / 2 ? r->ncols - nnum : 1;
    if (sym_width < 1) sym_width = 1;
    /* The symbol columns go direct (each string probes the import
     * dictionary) unless their strings would make it too big to probe at
     * random: then grouped (table/symgrp.h), column by column.  Decided from
     * their chunks' uncompressed bytes against a quarter of memory, and
     * then column by column: a column whose vocabulary (pq_chunk_ranges'
     * bound) is a small share of memory still goes direct, decoded once,
     * smallest first while those columns' vocabularies stay within a
     * sixteenth together.  The direct columns go first (the dictionary's
     * index serves only them), the grouped ones after them, which take
     * the direct ones' strings into their own index (ray_symgrp_adopt).
     * RAY_PQ_SYM_MODE=direct|grouped decides for every column instead, and
     * RAY_PQ_SYM_ORDER (rows|shards|freq|rowsflat) and RAY_PQ_SYM_GROUPS set
     * the grouped import's order of new positions and hash groups.  Debug
     * builds: RAY_PQ_SYM_RAM sets the memory these decisions assume. */
    pq_sym_mode mode = PQ_SYM_AUTO;
    {
        const char* e = getenv("RAY_PQ_SYM_MODE");
        if (e && !strcmp(e,"direct")) mode = PQ_SYM_DIRECT;
        else if (e && !strcmp(e,"grouped")) mode = PQ_SYM_GROUPED;
    }
    int64_t sram = ram;
#if defined(DEBUG)
    {
        const char* e = getenv("RAY_PQ_SYM_RAM");
        if (e && *e && strtoll(e,NULL,10) > 0) sram = strtoll(e,NULL,10);
    }
#endif
    simp = nnum < r->ncols ? ray_sym_domain_import(writers[order[nnum]].dom) : NULL;
    int64_t nd = r->ncols;   /* order[nnum, nd): direct symbol columns, [nd, ncols): grouped */
    if (mode == PQ_SYM_AUTO && simp) {
        int64_t text = 0;
        for (int64_t i = nnum; i < r->ncols; i++) text = bytes[order[i]] > INT64_MAX - text ? INT64_MAX : text + bytes[order[i]];
        mode = sram > 0 && text > sram / 4 ? PQ_SYM_GROUPED : PQ_SYM_DIRECT;
        if (mode == PQ_SYM_GROUPED) {
            /* the direct ones, marked by a negated vocabulary */
            int64_t sum = 0, ndirect = 0;
            for (;;) {
                int64_t best = -1;
                for (int64_t i = nnum; i < r->ncols; i++) {
                    int64_t c = order[i];
                    if (vocab[c] >= 0 && (best < 0 || vocab[c] < vocab[best])) best = c;
                }
                if (best < 0 || vocab[best] > sram / 64 || vocab[best] > sram / 16 - sum) break;
                sum += vocab[best]; vocab[best] = -1 - vocab[best]; ndirect++;
            }
            if (ndirect == r->ncols - nnum) mode = PQ_SYM_DIRECT;
            else {
                /* direct first, each part by size as before (a stable split) */
                int64_t at = nnum;
                for (int64_t i = nnum; i < r->ncols; i++) {
                    int64_t c = order[i];
                    if (vocab[c] >= 0) continue;
                    for (int64_t j = i; j > at; j--) order[j] = order[j-1];
                    order[at++] = c;
                }
                nd = at;
            }
            for (int64_t i = nnum; i < r->ncols; i++) if (vocab[order[i]] < 0) vocab[order[i]] = -1 - vocab[order[i]];
        }
    }
    if (mode == PQ_SYM_GROUPED && simp) {
        ray_symgrp_order_t so = RAY_SYMGRP_ROWS;
        const char* e = getenv("RAY_PQ_SYM_ORDER");
        if (e && !strcmp(e,"shards")) so = RAY_SYMGRP_SHARDS;
        else if (e && !strcmp(e,"freq")) so = RAY_SYMGRP_FREQ;
        else if (e && !strcmp(e,"rowsflat")) so = RAY_SYMGRP_ROWS_FLAT;
        sgrp = ray_symgrp_new(simp,so);
        if (!sgrp) { err = ray_error("oom",NULL); goto done; }
        if (nd == r->ncols) nd = nnum;   /* forced: every symbol column grouped */
    } else nd = r->ncols;
    if (trace && simp)
        for (int64_t i = nnum; i < r->ncols; i++) {
            ray_t* nm = ray_sym_str(r->schema[order[i]].name);
            fprintf(stderr,"parquet symcol: col=%.*s text_mb=%.3f vocab_mb=%.3f mode=%s\n",
                    (int)ray_str_len(nm),ray_str_ptr(nm),(double)bytes[order[i]]/1048576.0,
                    (double)vocab[order[i]]/1048576.0,i < nd ? "direct" : "grouped");
        }
    if (!sgrp && nnum < r->ncols && writers[order[nnum]].dom) {
        symf.dom = writers[order[nnum]].dom;
        if (ray_thread_create(&sym_thread,pq_symflush_fn,&symf) == RAY_OK) sym_running = true;
    }
    ray_symimp_stats_t ist;   /* the import dictionary's counts, per pass */
    if (trace && symf.dom) ray_sym_domain_import_stats(symf.dom,&ist);
    bool all = !colwise && !sgrp;
    for (int64_t p = 0; p < r->ncols && !err; p += work.npass) {
        int64_t wd = all ? r->ncols : p < nnum ? width : p < nd ? sym_width : 1;
        int64_t lim = all ? r->ncols : p < nnum ? nnum : p < nd ? nd : r->ncols;
        work.pass = order + p;
        work.npass = lim - p < wd ? lim - p : wd;
        int64_t tasks = r->ngroups*work.npass;
        int64_t t0 = trace ? ray_profile_now_ns() : 0, t1 = 0;
        if (sgrp && p >= nd) {
            /* memory for the pass: what is left under the anon watermark */
            int64_t wm = ray_heap_anon_watermark();
            int64_t budget = wm > 0 ? wm - ray_heap_anon_committed() : (int64_t)1 << 30;
            if (budget < ((int64_t)64 << 20)) budget = (int64_t)64 << 20;
            /* hash groups: a group's index table (~24 B an entry, the
             * column's rows bounding its new strings) within a quarter */
            int groups = 1;
            const char* e = getenv("RAY_PQ_SYM_GROUPS");
            if (e && *e && strtol(e,NULL,10) > 0) groups = (int)strtol(e,NULL,10);
            else {
                int64_t need = (ray_symgrp_entries(sgrp) + r->rows) * 24;
                while (groups < RAY_SYMGRP_MAX && need / groups > budget / 4) groups *= 2;
            }
            pq_sym_grouped(&work,sgrp,simp,work.pass[0],groups,budget,workers,trace);
        } else if (tasks > 0) {
            if (ray_pool_par_dispatch_ok(pool,tasks,2)) ray_pool_dispatch_n(pool,pq_write_direct_group,&work,(uint32_t)tasks);
            else pq_write_direct_group(&work,0,0,tasks);
        }
        for (int64_t t = 0; t < tasks; t++) if (errors[t]) {
            if (!err) err = errors[t]; else ray_error_free(errors[t]);
            errors[t] = NULL;
        }
        if (!err && ray_interrupted()) err = ray_error("cancel","parquet conversion interrupted");
        if (!err && atomic_load_explicit(&symf.err,memory_order_acquire) != RAY_OK)
            err = ray_error(ray_err_code_str((ray_err_t)atomic_load(&symf.err)),"parquet: cannot flush symbol file");
        if (err) break;
        if (trace) t1 = ray_profile_now_ns();
        for (int64_t i = 0; i < work.npass; i++) {
            int64_t c = work.pass[i];
            writers[c].rows = r->rows;
            writers[c].had_nulls = atomic_load_explicit(&nulls[c],memory_order_relaxed) != 0;
            if (all || writers[c].type == RAY_SYM) continue;
            ray_err_t e = ray_col_stream_close(&writers[c],false);
            if (e != RAY_OK) { err = ray_error(ray_err_code_str(e),"parquet: column close failed"); break; }
            ray_col_stream_hash_one(&writers[c]);
        }
        /* the workers' page and dictionary buffers grown past PQ_SPARE_MAX
         * for this pass's columns go with the pass */
        for (int64_t i = 0; i < nspare; i++) {
            if (spare[i].page_cap > PQ_SPARE_MAX) { ray_free_raw(spare[i].page); spare[i].page = NULL; spare[i].page_cap = 0; }
            if (spare[i].dict_cap > PQ_SPARE_MAX) { ray_free_raw(spare[i].dict); spare[i].dict = NULL; spare[i].dict_cap = 0; }
        }
        if (trace && !err) {
            ray_t* nm = ray_sym_str(r->schema[work.pass[0]].name);
            fprintf(stderr,"parquet pass: first=%.*s cols=%lld %s decode=%.1fms close=%.1fms\n",
                    (int)ray_str_len(nm),ray_str_ptr(nm),(long long)work.npass,
                    writers[work.pass[0]].type == RAY_SYM ? "sym" : "num",
                    (double)(t1-t0)/1e6,(double)(ray_profile_now_ns()-t1)/1e6);
            if (symf.dom && ray_sym_domain_import_stats(symf.dom,&ist) && ist.strings)
                fprintf(stderr,"parquet symimp: strings=%lld dedup=%lld probes=%lld hits=%lld added=%lld"
                        " slots=%lld false_tags=%lld grows=%lld grow_mb=%.1f"
                        " entries=%lld rec_mb=%.1f tab_mb=%.1f offc_mb=%.1f\n",
                        (long long)ist.strings,(long long)ist.dedup,(long long)ist.probes,
                        (long long)ist.hits,(long long)ist.added,(long long)ist.slots,
                        (long long)ist.false_tags,(long long)ist.grows,(double)ist.grow_bytes/1048576.0,
                        (long long)ist.count,(double)ist.rec_bytes/1048576.0,
                        (double)ist.tab_bytes/1048576.0,(double)ist.off_bytes/1048576.0);
        }
    }
    /* One pass: the columns finish side by side and their hash indexes are
     * built in waves while the symbol file appender is still writing (its
     * writeback can wait on the column data's); a symbol column's header
     * takes the domain's final count, which no longer changes.  The
     * column-by-column passes have closed and indexed theirs already. */
    if (all && !err) {
        int64_t tc = trace ? ray_profile_now_ns() : 0;
        ray_err_t e = ray_col_stream_close_all(writers,r->ncols,false,NULL);
        if (e != RAY_OK) err = ray_error(ray_err_code_str(e),"parquet: column close failed");
        else ray_col_stream_hash_all(writers,r->ncols,NULL);
        if (trace) fprintf(stderr,"parquet phase: close+hash=%.1fms\n",(double)(ray_profile_now_ns()-tc)/1e6);
    }
    if (sym_running) {
        atomic_store_explicit(&symf.stop,true,memory_order_release);
        ray_thread_join(sym_thread);
        if (!err && atomic_load(&symf.err) != RAY_OK)
            err = ray_error(ray_err_code_str((ray_err_t)atomic_load(&symf.err)),"parquet: cannot flush symbol file");
    }
done:
    for (int64_t c = 0; c < r->ncols; c++) r->schema[c].import_domain = NULL;
    ray_free_raw(offsets); ray_free_raw(nulls); ray_free_raw(locks); ray_free_raw(errors);
    ray_free_raw(lo); ray_free_raw(hi); ray_free_raw(bytes); ray_free_raw(vocab); ray_free_raw(order);
    if (prefetched) ray_free_raw((void*)prefetched);
    for (int64_t i = 0; i < nspare; i++) {
        ray_free_raw(spare[i].page); ray_free_raw(spare[i].dict); ray_free_raw(spare[i].symbols);
    }
    ray_free_raw(spare);
    ray_symgrp_free(sgrp);
    return err;
}
ray_t* ray_parquet_splayed_typed(const char* path, const char* dir, ray_t* types) {
#ifdef RAY_FUZZING
    return ray_error("restricted","Parquet imports disabled under fuzzing");
#endif
    char dest[1024];
    if (!pq_dest(dir,dest,sizeof(dest))) return pq_error("invalid or too long destination path");
    dir = dest;
    ray_parquet_t* r = NULL; ray_t* err = ray_parquet_open(path,NULL,PQ_BATCH,&r);
    if (err) return err;
    char staging[1100], spool[1200], leaf[1500], file[1700];
    ray_col_stream_t* writers = NULL; int64_t opened = 0, parts = 0;
    ray_err_t e = RAY_OK; ray_sym_domain_t* domain = NULL;
    err = pq_import_types(r,types);
    if (err) goto done;
    err = pq_stage(r,dir,staging,sizeof(staging),false);
    if (err) goto done;
    writers = ray_calloc_raw((size_t)r->ncols*sizeof(*writers));
    if (!writers) { err = ray_error("oom",NULL); goto done; }
    for (int64_t c = 0; c < r->ncols; c++) if (r->schema[c].native_symbol && !domain) {
        snprintf(file,sizeof(file),"%s/.sym",staging);
        /* The direct import (no STR column) builds the symbol file with the
         * import dictionary: strings go straight into the mapped file. */
        bool str_cols = false;
        for (int64_t k = 0; k < r->ncols; k++)
            if (!r->schema[k].native_symbol && r->schema[k].type == RAY_STR) str_cols = true;
        if (!str_cols) domain = ray_sym_domain_create_import(file);
        if (!domain) domain = ray_sym_domain_open_or_create(file);
        if (!domain) { err = ray_error("oom",NULL); goto done; }
        if (ray_sym_domain_intern(domain,"",0) != 0) { err = pq_error("cannot initialize native symbol domain"); goto done; }
    }
    for (int64_t c = 0; c < r->ncols; c++) {
        opened++;
        e = ray_col_stream_open(&writers[c],staging,r->schema[c].name,r->schema[c].native_symbol ? RAY_SYM : (int8_t)r->schema[c].type,domain);
        if (e != RAY_OK) goto io_fail;
    }
    for (int64_t c = 0; c < r->ncols; c++) {
        e = ray_col_stream_index_begin(&writers[c],0);
        if (e != RAY_OK) goto io_fail;
    }
    bool direct = true;
    for (int64_t c = 0; c < r->ncols; c++) if (writers[c].type == RAY_STR) direct = false;
    bool trace = getenv("RAY_CSV_TRACE") != NULL;
    int64_t tp = trace ? ray_profile_now_ns() : 0;
    if (direct) {
        err = pq_write_direct(r,writers);
        if (err) goto done;
        goto finish_columns;
    }
    /* Parallel decode spools bounded native batches to disk. Their ordered
     * merge never retains an entire row group or the full input in RAM. */
    snprintf(spool,sizeof(spool),"%s/.groups",staging);
    e = ray_mkdir(spool); if (e != RAY_OK) goto io_fail;
    err = pq_write_groups(r,spool,"batch",false,&parts);
    if (err) goto done;
    for (int64_t part = 0; part < parts; part++) {
        if (ray_interrupted()) { e = RAY_ERR_CANCEL; goto io_fail; }
        snprintf(leaf,sizeof(leaf),"%s/%lld/batch",spool,(long long)part);
        ray_t* batch = ray_read_splayed(leaf,NULL);
        if (!batch || RAY_IS_ERR(batch)) { err = batch ? batch : ray_error("oom",NULL); goto done; }
        for (int64_t c = 0; c < r->ncols; c++) {
            e = ray_col_stream_append(&writers[c],ray_table_get_col_idx(batch,c));
            if (e != RAY_OK) break;
        }
        ray_release(batch);
        if (e != RAY_OK) goto io_fail;
        /* Remove only files created by this import, after their mappings close. */
        for (int64_t c = 0; c <= r->ncols; c++) {
            const char* name = c == r->ncols ? ".d" : ray_str_ptr(ray_sym_str(r->schema[c].name));
            snprintf(file,sizeof(file),"%s/%s",leaf,name);
            if (remove(file)) { e = RAY_ERR_IO; goto io_fail; }
        }
        if (!pq_remove_dir(leaf)) { e = RAY_ERR_IO; goto io_fail; }
        snprintf(leaf,sizeof(leaf),"%s/%lld",spool,(long long)part);
        if (!pq_remove_dir(leaf)) { e = RAY_ERR_IO; goto io_fail; }
    }
    if (!pq_remove_dir(spool)) { e = RAY_ERR_IO; goto io_fail; }
finish_columns:
    if (trace) { fprintf(stderr,"parquet phase: passes=%.1fms\n",(double)(ray_profile_now_ns()-tp)/1e6); tp = ray_profile_now_ns(); }
    /* the direct import appended the symbols as they came: this writes
     * what is left and syncs the file */
    if (domain) { e = ray_sym_domain_flush_append(domain,true); if (e != RAY_OK) goto io_fail; }
    if (trace) { fprintf(stderr,"parquet phase: sym flush=%.1fms\n",(double)(ray_profile_now_ns()-tp)/1e6); tp = ray_profile_now_ns(); }
    /* All columns are still under a private staging directory (the direct
     * import has closed and indexed them already, but for the symbol
     * columns of its column-by-column passes). Commit their final images
     * together below, after the hash re-read of any column left.  Every
     * column finishes as a pool task; the renames run serially after. */
    e = ray_col_stream_close_all(writers,r->ncols,false,NULL);
    if (e != RAY_OK) goto io_fail;
    /* The persisted domain is flushed and every writer is closed (the zone
     * and dictionary indexes were built inline while streaming). Drop the
     * ingestion hash tables and string arena before any hash re-read of the
     * integer columns; reopening needs only the file-backed vocabulary. */
    if (domain) {
        ray_sym_domain_release(domain); domain = NULL;
        for (int64_t c = 0; c < opened; c++) writers[c].dom = NULL;
    }
    if (trace) { fprintf(stderr,"parquet phase: close=%.1fms\n",(double)(ray_profile_now_ns()-tp)/1e6); tp = ray_profile_now_ns(); }
    ray_col_stream_hash_all(writers,r->ncols,NULL);
    if (trace) { fprintf(stderr,"parquet phase: hash=%.1fms\n",(double)(ray_profile_now_ns()-tp)/1e6); tp = ray_profile_now_ns(); }
    ray_t* schema = ray_vec_new(RAY_STR,r->ncols);
    if (!schema || RAY_IS_ERR(schema)) { err = schema ? schema : ray_error("oom",NULL); goto done; }
    for (int64_t c = 0; c < r->ncols; c++) {
        ray_t* name = ray_sym_str(r->schema[c].name);
        ray_t* next = ray_str_vec_append(schema,ray_str_ptr(name),ray_str_len(name));
        if (!next || RAY_IS_ERR(next)) { ray_release(schema); err = next ? next : ray_error("oom",NULL); goto done; }
        schema = next;
    }
    snprintf(file,sizeof(file),"%s/.d",staging);
    e = ray_col_save_bulk(schema,file); ray_release(schema);
    if (e != RAY_OK) goto io_fail;
    /* The column files already carry their index regions. Flush the final
     * file images before the directory becomes visible under its published name. */
    for (int64_t c = 0; c <= r->ncols; c++) {
        snprintf(file,sizeof(file),"%s/.d",staging);
        const char* path = c == r->ncols ? file : writers[c].path;
        ray_fd_t fd = ray_file_open(path,RAY_OPEN_READ | RAY_OPEN_WRITE);
        if (fd == RAY_FD_INVALID) { e = RAY_ERR_IO; goto io_fail; }
        e = ray_file_sync(fd); ray_file_close(fd);
        if (e != RAY_OK) goto io_fail;
    }
    if (trace) { fprintf(stderr,"parquet phase: schema+sync=%.1fms\n",(double)(ray_profile_now_ns()-tp)/1e6); tp = ray_profile_now_ns(); }
    e = ray_file_sync_dir(file);
    if (e != RAY_OK) goto io_fail;
    err = pq_publish(staging,dir,r->rows);
    goto done;
io_fail:
    err = ray_error(ray_err_code_str(e),"parquet: splayed write failed; staging directory retained");
done:
    for (int64_t c = 0; c < opened; c++) ray_col_stream_abort(&writers[c]);
    ray_free_raw(writers); if (domain) ray_sym_domain_release(domain); ray_parquet_close(r); return err;
}

ray_t* ray_parquet_splayed(const char* path, const char* dir) {
    return ray_parquet_splayed_typed(path,dir,NULL);
}
static const char* pq_path_arg(ray_t* arg) {
    if (!arg || arg->type != -RAY_STR) return NULL;
    const char* p = ray_str_ptr(arg); size_t n = ray_str_len(arg);
    return p && n && !memchr(p,0,n) ? p : NULL;
}
ray_t* ray_parquet_metadata_fn(ray_t* path) {
    const char* p = pq_path_arg(path);
    return p ? ray_parquet_metadata(p) : ray_error("type","parquet.meta: path must be a nonempty string");
}
ray_t* ray_parquet_read_fn(ray_t** args, int64_t n) {
    if (n != 1 && n != 2) return ray_error("arity","parquet.read: [columns] path");
    const char* p = pq_path_arg(args[n-1]);
    return p ? ray_parquet_read(p,n == 2 ? args[0] : NULL) : ray_error("type","parquet.read: path must be a nonempty string");
}
ray_t* ray_parquet_splayed_fn(ray_t** args, int64_t n) {
    if (n != 2 && n != 3) return ray_error("arity","parquet.splayed: [types] path directory");
    const char* path = pq_path_arg(args[n-2]); const char* dir = pq_path_arg(args[n-1]);
    return path && dir ? ray_parquet_splayed_typed(path,dir,n == 3 ? args[0] : NULL) : ray_error("type","parquet.splayed: expected two paths");
}
ray_t* ray_parquet_parted_fn(ray_t** args, int64_t n) {
    if (n != 3 && n != 4) return ray_error("arity","parquet.parted: [types] path root table-name");
    const char* p = pq_path_arg(args[n-3]); const char* root = pq_path_arg(args[n-2]);
    ray_t* name = args[n-1]->type == -RAY_SYM ? ray_sym_str(args[n-1]->i64) : args[n-1];
    const char* table = pq_path_arg(name);
    return p && root && table ? ray_parquet_parted_typed(p,root,table,n == 4 ? args[0] : NULL) : ray_error("type","parquet.parted: expected paths and table name");
}
ray_t* ray_parquet_each_fn(ray_t** args, int64_t n) {
    if (n != 2 && n != 3) return ray_error("arity","parquet.each: [columns] path callback");
    const char* p = pq_path_arg(args[n-2]); ray_t* fn = args[n-1];
    if (!p || (fn->type != RAY_LAMBDA && fn->type != RAY_UNARY && fn->type != RAY_VARY))
        return ray_error("type","parquet.each: expected path and callable");
    ray_parquet_t* r = NULL; ray_t* err = pq_open_options(p,n == 3 ? args[0] : NULL,&r);
    if (err) return err;
    int64_t rows = 0;
    for (;;) {
        ray_t* batch = ray_parquet_next(r);
        if (!batch) break;
        if (RAY_IS_ERR(batch)) { err = batch; break; }
        rows += ray_table_nrows(batch);
        ray_t* result = call_fn1(fn,batch); ray_release(batch);
        if (!result || RAY_IS_ERR(result)) { err = result ? result : ray_error("oom",NULL); break; }
        ray_release(result);
    }
    ray_parquet_close(r); return err ? err : ray_i64(rows);
}

/* Reopenable lazy source. The descriptor contains data only, never a raw
 * pointer; select checks restricted mode again before opening the file. */
ray_t* ray_parquet_scan_fn(ray_t** args, int64_t n) {
    if (n < 1 || n > 2) return ray_error("arity","parquet.scan: [options] path");
    ray_t* path = args[n-1];
    if (!pq_path_arg(path)) return ray_error("type","parquet.scan: path must be a nonempty string");
    if (n == 2) {
        ray_t* opts = args[0];
        ray_t* keys = opts->type == RAY_DICT ? ray_dict_keys(opts) : NULL;
        if (!keys || keys->type != RAY_SYM) return pq_error("lazy scan options must be {types: [...]}");
        for (int64_t i = 0; i < keys->len; i++) {
            ray_t* k = ray_sym_vec_cell(keys,i);
            if (!k || (strcmp(ray_str_ptr(k),"types") && strcmp(ray_str_ptr(k),"strict"))) return pq_error("unknown lazy scan option");
        }
    }
    int64_t names[] = {ray_sym_intern(".parquet.source",15),ray_sym_intern(".parquet.options",16)};
    ray_t* keys = ray_vec_from_raw(RAY_SYM,names,n);
    ray_t* vals = ray_list_new(n);
    if (!keys || RAY_IS_ERR(keys) || !vals || RAY_IS_ERR(vals)) {
        if (keys) ray_release(keys);
        if (vals) ray_release(vals);
        return ray_error("oom",NULL);
    }
    ray_retain(path); ((ray_t**)ray_data(vals))[0] = path; vals->len = 1;
    if (n == 2) { ray_retain(args[0]); ((ray_t**)ray_data(vals))[1] = args[0]; vals->len = 2; }
    return ray_dict_new(keys,vals);
}
static const char* pq_symbol_name(ray_t* x) {
    return x && x->type == -RAY_SYM ? ray_str_ptr(ray_sym_str(x->i64)) : NULL;
}
static const char* pq_builtin_name(ray_t* x) {
    const char* name = pq_symbol_name(x);
    ray_t* value = name ? ray_env_get(x->i64) : NULL;
    return value && value->type == RAY_LAMBDA ? NULL : name;
}
static bool pq_reserved(const char* k) {
    return !strcmp(k,"from") || !strcmp(k,"where") || !strcmp(k,"by") ||
        !strcmp(k,"asc") || !strcmp(k,"desc") || !strcmp(k,"take") || !strcmp(k,"nearest");
}
static int64_t pq_column_id(ray_parquet_t* r, ray_t* x) {
    if (x && x->type == -RAY_SYM)
        for (int64_t c = 0; c < r->ncols; c++) if (r->schema[c].name == x->i64) return c;
    return -1;
}
/* Only statically transparent expressions permit projection. Unknown calls,
 * lambdas and dynamic evaluation may reference columns through local scope. */
static bool pq_references(ray_parquet_t* r, ray_t* e, uint8_t* keep, unsigned depth) {
    if (!e || depth > 64) return false;
    if (e->type == -RAY_SYM) {
        int64_t c = pq_column_id(r,e); if (c >= 0) keep[c] = 1;
        return true;
    }
    if (e->type == RAY_SYM) {
        for (int64_t i = 0; i < e->len; i++) {
            ray_t* cell = ray_sym_vec_cell(e,i);
            const char* name = cell ? ray_str_ptr(cell) : NULL;
            if (!name) return false;
            for (int64_t c = 0; c < r->ncols; c++)
                if (!strcmp(name,ray_str_ptr(ray_sym_str(r->schema[c].name)))) keep[c] = 1;
        }
        return true;
    }
    if (e->type == RAY_DICT) {
        ray_t* vals = ray_dict_vals(e);
        if (!vals || vals->type != RAY_LIST) return false;
        for (int64_t i = 0; i < vals->len; i++)
            if (!pq_references(r,ray_list_get(vals,i),keep,depth+1)) return false;
        return true;
    }
    if (e->type == RAY_LIST) {
        if (!e->len) return true;
        const char* fn = pq_builtin_name(ray_list_get(e,0));
        static const char* const transparent[] = {
            "+","-","*","/","mod","%","div","==","!=","<","<=",">",">=","and","or","not",
            "sum","count","avg","min","max","first","last","distinct","all","any",
            "abs","sqrt","floor","ceil","round","log","exp","like","within","in",
            "strlen","lower","upper","year","month","day","hour","minute","second","xbar",
            "if","nil?","substr","str-find"
        };
        if (fn && !strcmp(fn,"let")) {
            if (e->len != 4) return false;
            const char* binding = pq_symbol_name(ray_list_get(e,1));
            if (!binding || !strcmp(binding,"let")) return false;
            for (size_t i = 0; i < sizeof(transparent)/sizeof(*transparent); i++)
                if (!strcmp(binding,transparent[i])) return false;
            return pq_references(r,ray_list_get(e,2),keep,depth+1) &&
                pq_references(r,ray_list_get(e,3),keep,depth+1);
        }
        bool known = false;
        for (size_t i = 0; fn && i < sizeof(transparent)/sizeof(*transparent); i++)
            if (!strcmp(fn,transparent[i])) { known = true; break; }
        if (!known) return false;
        /* A source column shadows a function name in query scope. */
        if (pq_column_id(r,ray_list_get(e,0)) >= 0) return false;
        for (int64_t i = 1; i < e->len; i++)
            if (!pq_references(r,ray_list_get(e,i),keep,depth+1)) return false;
        return true;
    }
    return e->type < RAY_TABLE;
}
/* A pushable WHERE must be entirely row-local comparisons. In particular,
 * prefiltering an aggregate/vector-dependent predicate changes its meaning. */
static bool pq_query_range(ray_parquet_t* r, ray_t* e, int64_t* column, int64_t* lo, int64_t* hi, unsigned depth) {
    if (!e || e->type != RAY_LIST || e->len < 3 || depth > 64) return false;
    const char* fn = pq_builtin_name(ray_list_get(e,0));
    if (!fn || pq_column_id(r,ray_list_get(e,0)) >= 0) return false;
    if (!strcmp(fn,"and")) {
        for (int64_t i = 1; i < e->len; i++)
            if (!pq_query_range(r,ray_list_get(e,i),column,lo,hi,depth+1)) return false;
        return true;
    }
    if (e->len != 3) return false;
    ray_t* a = ray_list_get(e,1); ray_t* b = ray_list_get(e,2);
    int64_t c = pq_column_id(r,a); bool reverse = false;
    if (c < 0) { c = pq_column_id(r,b); b = a; reverse = true; }
    bool within = !strcmp(fn,"within") && !reverse && b->type == RAY_I64 && b->len == 2;
    if (c < 0 || (!within && (b->type != -RAY_I64 || b->i64 == INT64_MIN))) return false;
    pq_schema* s = &r->schema[c];
    if ((s->type != RAY_I16 && s->type != RAY_I32 && s->type != RAY_I64) ||
        (s->converted != -1 && (s->converted < 15 || s->converted > 18))) return false;
    int64_t l = INT64_MIN, h = INT64_MAX, x = within ? 0 : b->i64;
    if (within) {
        l = ((int64_t*)ray_data(b))[0]; h = ((int64_t*)ray_data(b))[1];
        if (l == INT64_MIN || h == INT64_MIN || l > h) return false;
    } else if (!strcmp(fn,"==")) l = h = x;
    else if (!strcmp(fn,reverse ? ">=" : "<=")) h = x;
    else if (!strcmp(fn,reverse ? ">" : "<")) h = x-1;
    else if (!strcmp(fn,reverse ? "<=" : ">=")) l = x;
    else if (!strcmp(fn,reverse ? "<" : ">")) { if (x == INT64_MAX) return false; l = x+1; }
    else return false;
    if (*column < 0) *column = c;
    if (*column == c) { if (l > *lo) *lo = l; if (h < *hi) *hi = h; }
    return true;
}
/* The existing aggregation registry supplies the same null and type rules as
 * native group execution. This narrow plan has one global group and bounded
 * state; expressions, grouping and ordering retain the general query path. */
typedef struct {
    int64_t name, pos;
    const agg_vtable_t* kernel;
    void* state;
    bool count;
} pq_aggregate;
static bool pq_exact_range(ray_parquet_t* r, ray_t* where) {
    if (!where) return true;
    if (r->filter_pos < 0 || where->type != RAY_LIST) return false;
    const char* fn = pq_symbol_name(ray_list_get(where,0));
    if (fn && !strcmp(fn,"and")) {
        for (int64_t i = 1; i < where->len; i++) if (!pq_exact_range(r,ray_list_get(where,i))) return false;
        return true;
    }
    int64_t c = r->selected[r->filter_pos];
    return where->len == 3 && (pq_column_id(r,ray_list_get(where,1)) == c || pq_column_id(r,ray_list_get(where,2)) == c);
}
typedef struct { int64_t pos, lo, hi; } pq_predicate;
static bool pq_predicates(ray_parquet_t* r, ray_t* where, pq_predicate* predicates, int64_t* n) {
    if (!where) return true;
    if (where->type != RAY_LIST) return false;
    const char* fn = pq_symbol_name(ray_list_get(where,0));
    if (fn && !strcmp(fn,"and")) {
        for (int64_t i = 1; i < where->len; i++)
            if (!pq_predicates(r,ray_list_get(where,i),predicates,n)) return false;
        return true;
    }
    int64_t column = -1, lo = INT64_MIN, hi = INT64_MAX, pos = 0;
    if (*n == PQ_MAX_COLS || !pq_query_range(r,where,&column,&lo,&hi,0) || column < 0) return false;
    while (pos < r->noutput && r->selected[pos] != column) pos++;
    if (pos == r->noutput) return false;
    predicates[(*n)++] = (pq_predicate){pos,lo,hi}; return true;
}
static void pq_filter_integers(ray_t* batch, const pq_predicate* predicates, int64_t n) {
    int64_t rows = ray_table_nrows(batch), dst = 0, nc = ray_table_ncols(batch);
    for (int64_t i = 0; i < rows; i++) {
        bool keep = true;
        for (int64_t p = 0; p < n && keep; p++) {
            ray_t* v = ray_table_get_col_idx(batch,predicates[p].pos);
            int64_t x = v->type == RAY_I16 ? ((int16_t*)ray_data(v))[i] :
                v->type == RAY_I32 ? ((int32_t*)ray_data(v))[i] : ((int64_t*)ray_data(v))[i];
            keep = ray_vec_is_null(v,i) ? predicates[p].lo == INT64_MIN :
                x >= predicates[p].lo && x <= predicates[p].hi;
        }
        if (!keep) continue;
        if (dst != i) for (int64_t c = 0; c < nc; c++) {
            ray_t* v = ray_table_get_col_idx(batch,c); size_t size = ray_sym_elem_size(v->type,v->attrs);
            memcpy((uint8_t*)ray_data(v)+(size_t)dst*size,(uint8_t*)ray_data(v)+(size_t)i*size,size);
        }
        dst++;
    }
    for (int64_t c = 0; c < nc; c++) ray_table_get_col_idx(batch,c)->len = dst;
}
static ray_t* pq_accumulate(ray_parquet_t* r, pq_aggregate* aggs, int64_t n,
                            const uint32_t* gids, const pq_predicate* predicates, int64_t np) {
    for (;;) {
        ray_t* batch = ray_parquet_next(r);
        if (!batch || RAY_IS_ERR(batch)) return batch;
        if (np) pq_filter_integers(batch,predicates,np);
        int64_t rows = ray_table_nrows(batch);
        for (int64_t a = 0; a < n; a++) {
            ray_t* v = ray_table_get_col_idx(batch,aggs[a].pos);
            ray_valid_t valid = {ray_data(v),v->type,ray_vec_may_have_nulls(v)};
            aggs[a].kernel->update_batch(aggs[a].state,aggs[a].kernel->state_size,gids,ray_data(v),&valid,rows,NULL);
        }
        ray_release(batch);
    }
}
typedef struct {
    ray_parquet_t* parent;
    int64_t first, n, np;
    pq_aggregate *templates, **states;
    const uint32_t* gids;
    const pq_predicate* predicates;
    ray_t** errors;
} pq_aggregate_work;
static void pq_aggregate_group(void* ptr, uint32_t worker, int64_t start, int64_t end) {
    (void)worker; pq_aggregate_work* w = ptr;
    for (int64_t i = start; i < end; i++) {
        ray_parquet_t* r = pq_group_reader(w->parent,w->first+i,w->parent->nselected);
        pq_aggregate* aggs = ray_calloc_raw((size_t)w->n*sizeof(*aggs));
        w->states[i] = aggs;
        if (!r || !aggs) { w->errors[i] = ray_error("oom",NULL); ray_parquet_close(r); continue; }
        for (int64_t a = 0; a < w->n; a++) {
            aggs[a] = w->templates[a]; aggs[a].state = ray_alloc_raw(aggs[a].kernel->state_size);
            if (!aggs[a].state) { w->errors[i] = ray_error("oom",NULL); break; }
            aggs[a].kernel->init(aggs[a].state);
        }
        if (!w->errors[i]) w->errors[i] = pq_accumulate(r,aggs,w->n,w->gids,w->predicates,w->np);
        ray_parquet_close(r);
    }
}
static ray_t* pq_aggregate_groups(ray_parquet_t* r, pq_aggregate* aggs, int64_t n,
                                  const uint32_t* gids, const pq_predicate* predicates, int64_t np) {
    ray_pool_t* pool = pq_group_pool(r);
    if (!pool) return pq_accumulate(r,aggs,n,gids,predicates,np);
    uint32_t width = ray_pool_total_workers(pool);
    if (width > RAY_POOL_INIT_TASKS) width = RAY_POOL_INIT_TASKS;
    pq_aggregate** states = ray_calloc_raw((size_t)width*sizeof(*states));
    ray_t** errors = ray_calloc_raw((size_t)width*sizeof(*errors));
    ray_t* err = NULL;
    if (!states || !errors) { err = ray_error("oom",NULL); goto done; }
    for (int64_t first = 0; first < r->ngroups && !err; first += width) {
        uint32_t count = r->ngroups-first < width ? (uint32_t)(r->ngroups-first) : width;
        memset(states,0,(size_t)count*sizeof(*states)); memset(errors,0,(size_t)count*sizeof(*errors));
        pq_aggregate_work work = {r,first,n,np,aggs,states,gids,predicates,errors};
        ray_pool_dispatch_n(pool,pq_aggregate_group,&work,count);
        for (uint32_t i = 0; i < count; i++) {
            if (errors[i]) { if (!err) err = errors[i]; else ray_error_free(errors[i]); }
            if (!states[i]) { if (!err) err = ray_error("cancel","parquet scan interrupted"); continue; }
            for (int64_t a = 0; a < n; a++) {
                if (!err) aggs[a].kernel->merge(aggs[a].state,states[i][a].state,NULL);
                ray_free_raw(states[i][a].state);
            }
            ray_free_raw(states[i]);
        }
    }
done:
    ray_free_raw(states); ray_free_raw(errors); return err;
}
static ray_t* pq_stream_aggregates(ray_parquet_t* r, ray_t* query, ray_t* where) {
    /* A configured standalone LIKE is already fully applied by the reader.
     * Otherwise range() proves row-local integer comparisons; other columns
     * remain residual filters. Keep the aggregate row even for zero matches. */
    if (where && r->filter_pos < 0 && !r->text_pattern) return NULL;
    bool residual = !r->text_pattern && !pq_exact_range(r,where);
    pq_predicate predicates[PQ_MAX_COLS]; int64_t np = 0;
    if (residual && !pq_predicates(r,where,predicates,&np)) return NULL;
    ray_t* keys = ray_dict_keys(query); ray_t* vals = ray_dict_vals(query);
    if (!keys || keys->type != RAY_SYM || !vals || vals->type != RAY_LIST || keys->len > PQ_MAX_COLS) return NULL;
    pq_aggregate* aggs = ray_calloc_raw((size_t)keys->len*sizeof(*aggs));
    if (!aggs) return ray_error("oom",NULL);
    int64_t n = 0; bool all_count = true;
    ray_t* result = NULL; uint32_t* gids = NULL;
    for (int64_t i = 0; i < keys->len; i++) {
        ray_t* key = ray_sym_vec_cell(keys,i); const char* k = key ? ray_str_ptr(key) : NULL;
        if (!k) goto done;
        if (!strcmp(k,"from") || !strcmp(k,"where")) continue;
        if (pq_reserved(k)) goto done;
        ray_t* e = ray_list_get(vals,i);
        if (!e || e->type != RAY_LIST || e->len != 2) goto done;
        const char* fn = pq_builtin_name(ray_list_get(e,0));
        int64_t col = pq_column_id(r,ray_list_get(e,1));
        if (!fn || col < 0 || pq_column_id(r,ray_list_get(e,0)) >= 0) goto done;
        uint16_t op = !strcmp(fn,"count") ? OP_COUNT : !strcmp(fn,"sum") ? OP_SUM :
            !strcmp(fn,"avg") ? OP_AVG : !strcmp(fn,"min") ? OP_MIN : !strcmp(fn,"max") ? OP_MAX : 0;
        int8_t type = (int8_t)r->schema[col].type;
        /* Compact dictionary ids are not raw STR slots or runtime SYM ids.
         * Count is representation-independent; lexical extrema use the
         * general executor, which resolves the attached domain. */
        if (r->schema[col].native_symbol) {
            if (op != OP_COUNT) goto done;
            type = RAY_I32;
        }
        /* Floating summation order can differ between engines. Keep that
         * execution choice in the general planner; integer averages are exact. */
        if (!op || ((op == OP_SUM || op == OP_AVG) && type != RAY_I16 && type != RAY_I32 && type != RAY_I64)) goto done;
        const agg_vtable_t* kernel = agg_resolve(op,type);
        if (!kernel || kernel->kind != ACC_STREAMING || !kernel->finalize_value || !kernel->update_batch || !kernel->merge) goto done;
        int64_t pos = 0;
        while (pos < r->noutput && r->selected[pos] != col) pos++;
        if (pos == r->noutput) goto done;
        aggs[n] = (pq_aggregate){ray_sym_intern(k,strlen(k)),pos,kernel,NULL,op == OP_COUNT};
        if (op != OP_COUNT) all_count = false;
        n++;
    }
    if (!n) goto done;
    for (int64_t a = 0; a < n; a++) {
        aggs[a].state = ray_alloc_raw(aggs[a].kernel->state_size);
        if (!aggs[a].state) { result = ray_error("oom",NULL); goto done; }
        aggs[a].kernel->init(aggs[a].state);
    }
    if (!(all_count && !where)) {
        gids = ray_calloc_raw(PQ_BATCH*sizeof(*gids));
        if (!gids) { result = ray_error("oom",NULL); goto done; }
        result = pq_aggregate_groups(r,aggs,n,gids,predicates,np);
        if (result) goto done;
    }
    result = ray_table_new(n);
    if (!result) result = ray_error("oom",NULL);
    if (RAY_IS_ERR(result)) goto done;
    for (int64_t a = 0; a < n; a++) {
        ray_t* v = ray_vec_new(aggs[a].kernel->out_type,1);
        if (!v || RAY_IS_ERR(v)) { ray_release(result); result = v ? v : ray_error("oom",NULL); goto done; }
        v->len = 1;
        if (all_count && !where) ((int64_t*)ray_data(v))[0] = r->rows;
        else if (aggs[a].kernel->finalize_value(aggs[a].state,ray_data(v))) v->attrs |= RAY_ATTR_HAS_NULLS;
        result = ray_table_add_col(result,aggs[a].name,v); ray_release(v);
        if (!result) result = ray_error("oom",NULL);
        if (RAY_IS_ERR(result)) break;
    }
done:
    for (int64_t a = 0; a < n; a++) ray_free_raw(aggs[a].state);
    ray_free_raw(aggs); ray_free_raw(gids);
    return result;
}
ray_t* ray_parquet_select_source(ray_t* source, ray_t* query, bool* complete) {
    *complete = false;
    if (!source || source->type != RAY_DICT || ray_dict_len(source) < 1 || ray_dict_len(source) > 2) return NULL;
    ray_t* path = pq_option(source,".parquet.source");
    if (!path) return NULL;
    if (ray_eval_get_restricted()) { ray_release(path); return ray_error("access","restricted"); }
    const char* p = pq_path_arg(path);
    if (!p) { ray_release(path); return pq_error("invalid lazy source path"); }
    ray_parquet_t* r = NULL;
    ray_t* err = ray_parquet_open(p,NULL,PQ_BATCH,&r); ray_release(path);
    if (err) return err;
    ray_t* options = pq_option(source,".parquet.options");
    if (options) {
        ray_t* types = options->type == RAY_DICT ? pq_option(options,"types") : NULL;
        err = options->type == RAY_DICT ? pq_scan_types(r,types) : pq_error("invalid lazy scan options");
        if (!err) err = pq_strict_option(r,options);
        if (types) ray_release(types);
        ray_release(options);
        if (err) { ray_parquet_close(r); return err; }
    }
    uint8_t* keep = ray_calloc_raw((size_t)r->ncols);
    if (!keep) { ray_parquet_close(r); return ray_error("oom",NULL); }
    ray_t* keys = ray_dict_keys(query); ray_t* vals = ray_dict_vals(query);
    bool project = keys && keys->type == RAY_SYM && vals && vals->type == RAY_LIST;
    bool explicit = false;
    for (int64_t i = 0; project && i < keys->len; i++) {
        ray_t* key = ray_sym_vec_cell(keys,i); const char* k = key ? ray_str_ptr(key) : NULL;
        if (!k) { project = false; break; }
        if (!strcmp(k,"from")) continue;
        if (!pq_reserved(k)) explicit = true;
        project = pq_references(r,ray_list_get(vals,i),keep,0);
    }
    if (project && explicit) {
        int64_t n = 0;
        for (int64_t c = 0; c < r->ncols; c++) if (keep[c]) r->selected[n++] = (int32_t)c;
        /* Keep a column for row cardinality when outputs contain only constants. */
        if (!n) { r->selected[0] = 0; n = 1; }
        r->nselected = r->noutput = n;
    }
    ray_free_raw(keep);
    ray_t* where = pq_option(query,"where");
    int64_t col = -1, lo = INT64_MIN, hi = INT64_MAX;
    if (pq_query_range(r,where,&col,&lo,&hi,0) && col >= 0 && lo <= hi) {
        err = ray_parquet_range(r,r->schema[col].name,lo,hi);
        /* Query bounds use INT64_MIN only for an absent lower bound: literal
         * nulls are rejected by pq_query_range. Rayforce nulls sort first. */
        if (!err) r->filter_nulls = lo == INT64_MIN;
    }
    /* A standalone LIKE is row-local. Retain the ordinary WHERE as well so
     * all later query stages use the same language semantics. */
    if (!err && where && where->type == RAY_LIST && where->len == 3) {
        const char* fn = pq_builtin_name(ray_list_get(where,0));
        int64_t c = pq_column_id(r,ray_list_get(where,1));
        ray_t* pat = ray_list_get(where,2);
        if (fn && !strcmp(fn,"like") && pq_column_id(r,ray_list_get(where,0)) < 0 &&
            c >= 0 && r->schema[c].type == RAY_STR && pat && pat->type == -RAY_STR) {
            for (int64_t i = 0; i < r->nselected; i++) if (r->selected[i] == c) {
                r->text_pos = i; r->text_pattern = pat; ray_retain(pat); break;
            }
        }
    }
    ray_t* streamed = err ? NULL : pq_stream_aggregates(r,query,where);
    if (where) ray_release(where);
    if (err) { ray_parquet_close(r); return err; }
    if (streamed) { *complete = true; ray_parquet_close(r); return streamed; }
    return pq_materialize(r);
}
