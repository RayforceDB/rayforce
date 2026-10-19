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

#ifndef RAY_ENC_H
#define RAY_ENC_H

/* Lightweight column encodings over 1024-value vectors.  Pure codec
 * functions over arrays of T-bit patterns held in uint64_t; no file or
 * format knowledge.  The byte grammar is normative: see the spec
 * docs/superpowers/specs/2026-10-05-compressed-columns-design.md,
 * "Encodings". */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "store/fsst.h"

#define RAY_ENC_VEC        1024
#define RAY_ENC_CHUNK      65536
#define RAY_ENC_DEPTH_MAX  3

/* ---- bit packing (interleaved layout) ----
 * Value i of 1024 belongs to lane i % L, row i / L, L = 1024 / T.  Each
 * lane packs its T values in row order, least-significant bit first, into
 * w consecutive T-bit words; word k of lane l is stored at word index
 * k*L + l.  w == 0 stores nothing. */
size_t ray_enc_packed_bytes(int w);
void   ray_enc_pack(const uint64_t* u, int T, int w, uint8_t* out);
void   ray_enc_unpack(const uint8_t* in, int T, int w, uint64_t* u);
int    ray_enc_bitlen(uint64_t x);

/* ---- kinds and logical values ---- */
typedef struct { uint8_t T; bool is_signed; bool is_float; } ray_enc_kind_t;

bool    ray_enc_kind_of(int8_t type, uint8_t attrs, ray_enc_kind_t* k);
/* The value kind of a type: ray_enc_kind_of for integer kinds, a float
 * kind ({32|64, false, true}) for F32 / F64; false otherwise. */
bool    ray_enc_value_kind(int8_t type, uint8_t attrs, ray_enc_kind_t* k);
bool    ray_enc_has_bitmap(int8_t type);
/* A float kind's T-bit pattern (T = 32 or 64) as a double: F32 widened,
 * which is exact.  The one helper every float path reads values with. */
static inline double ray_enc_fval(uint64_t b, int T) {
    if (T == 32) { uint32_t u = (uint32_t)b; float f; memcpy(&f, &u, 4); return (double)f; }
    double d; memcpy(&d, &b, 8); return d;
}
int64_t ray_enc_logical(uint64_t bits, ray_enc_kind_t k);
/* a < b in kind order; on a float kind this is dictionary order: numeric
 * on the value (F32 widened), -0.0 before +0.0.  The float order is defined
 * on non-NaN values only (a NaN is not ordered, so it breaks a sort or a
 * binary search): float callers (training, dict_codes, stats) must drop
 * NaN / null rows first. */
bool    ray_enc_lt(uint64_t a, uint64_t b, ray_enc_kind_t k);

/* One vector of input: T-bit patterns, null bitmap (bit i set = row i
 * null, bitmap-carrying types only) and the null count (in-band nulls of
 * SYM are counted too, with no bitmap). */
typedef struct {
    uint64_t       bits[RAY_ENC_VEC];
    uint8_t        nullmap[RAY_ENC_VEC / 8];
    int            n;
    int            n_nulls;
    ray_enc_kind_t k;
    bool           bitmap;
} ray_enc_input_t;

/* n must be 0..RAY_ENC_VEC; otherwise returns false. */
bool ray_enc_load(const void* data, int8_t type, uint8_t attrs,
                  int64_t start, int n, ray_enc_input_t* in);

typedef struct { uint64_t min, max; int runs; bool all_equal; } ray_enc_stats_t;
void ray_enc_stats(const uint64_t* v, int n, ray_enc_kind_t k, ray_enc_stats_t* s);

/* ---- codec types ---- */
typedef enum {
    RAY_ENC_RAW = 0, RAY_ENC_CONST = 1, RAY_ENC_FOR_BP = 2, RAY_ENC_DELTA_BP = 3,
    RAY_ENC_RLE = 4, RAY_ENC_DICT = 5, RAY_ENC_ALP = 6, RAY_ENC_ALP_RD = 7,
    RAY_ENC_STR_DICT = 8, RAY_ENC_FSST = 9
} ray_enc_code_t;

/* Input contract for every encoder entry point (choose / write / widths):
 * values are T-bit patterns held in uint64_t (T in {8,16,32,64}), n in
 * 0..1024.  Bits above T are ignored by FOR_BP / DELTA_BP, which emit
 * masked bases and starts, so a sign-extended input still yields the
 * canonical body; RLE and DICT compare whole words and expect canonical
 * (masked) input.  Packed widths satisfy w <= T. */

/* Growable output buffer over the buddy heap. */
typedef struct { uint8_t* p; size_t len, cap; bool oom; } ray_enc_buf_t;
void ray_enc_buf_free(ray_enc_buf_t* b);
bool ray_enc_buf_put(ray_enc_buf_t* b, const void* src, size_t n);
bool ray_enc_buf_pad8(ray_enc_buf_t* b);

/* Body sizes in bytes (codec bodies are 8-aligned). */
int64_t ray_enc_size_raw(int n, int T);
int64_t ray_enc_size_const(void);
int     ray_enc_for_width(const uint64_t* v, int n, ray_enc_kind_t k, uint64_t* base);
int64_t ray_enc_size_for(int w);
int     ray_enc_delta_width(const uint64_t* v, int n, ray_enc_kind_t k, uint64_t* dbase);
int64_t ray_enc_size_delta(int T, int w);

/* Child slots (spec "Child slots").  The top-level vector is depth 0. */
typedef enum {
    RAY_ENC_SLOT_TOP = 0,
    RAY_ENC_SLOT_RLE_VALUES,   /* kind: element type at depth 1, else the enclosing slot's kind */
    RAY_ENC_SLOT_RLE_ENDS,     /* kind U16 */
    RAY_ENC_SLOT_CODES,        /* kind U32 (DICT / STR_DICT codes) */
    RAY_ENC_SLOT_ALP_INTS,     /* kind I64 (spec erratum E-P5-1: the slot table says U64) */
    RAY_ENC_SLOT_ALP_RD_LEFT,  /* kind U8 */
    RAY_ENC_SLOT_FSST_LENGTHS  /* kind U32: RAW, FOR_BP */
} ray_enc_slot_t;

typedef struct { const uint64_t* v; uint32_t n; } ray_enc_dict_t;
typedef struct { uint8_t enc, width; int64_t bytes; } ray_enc_choice_t;

ray_enc_choice_t ray_enc_choose(const uint64_t* v, int n, ray_enc_kind_t k,
                                ray_enc_slot_t slot, int depth,
                                const ray_enc_dict_t* dict);
bool    ray_enc_write(const uint64_t* v, int n, ray_enc_kind_t k,
                      ray_enc_slot_t slot, int depth, const ray_enc_dict_t* dict,
                      ray_enc_buf_t* out, ray_enc_choice_t* chosen);
/* Write a top-level body in encoding `enc` (children still go through
 * selection).  False when `enc` is not legal for the kind / dictionary,
 * not applicable to the values (CONST on unequal values, DICT on a value
 * outside the dictionary), or on OOM. */
bool    ray_enc_write_as(uint8_t enc, const uint64_t* v, int n, ray_enc_kind_t k,
                         const ray_enc_dict_t* dict, ray_enc_buf_t* out, ray_enc_choice_t* chosen);
/* Size of a top-level body in `enc` (RAW, CONST, FOR_BP, DELTA_BP, RLE,
 * DICT), or -1 when not legal or not applicable. */
int64_t ray_enc_body_size(uint8_t enc, const uint64_t* v, int n, ray_enc_kind_t k,
                          const ray_enc_dict_t* dict);
int64_t ray_enc_read(const uint8_t* p, size_t avail, uint8_t enc, uint8_t width,
                     int n, ray_enc_kind_t k, ray_enc_slot_t slot, int depth,
                     const ray_enc_dict_t* dict, uint64_t* out);
/* Why the last ray_enc_read on this thread failed (diagnostics only). */
const char* ray_enc_read_why(void);
/* Sets the reason ray_enc_read_why reports and returns -1: for readers
 * outside enc_codec.c (ALP, ALP_RD). */
int64_t ray_enc_read_fail(const char* why);
/* Distinct sorted values of v in the logical order of k (ray_enc_lt; on a
 * float kind v must hold no NaN, see ray_enc_lt).  Returns 1 =
 * trained (*out holds *out_n values; free with ray_free_raw), 0 = no
 * dictionary (n <= 0 or more than 1<<16 distinct values), -1 = out of
 * memory.  *out is untouched unless 1 is returned. */
int     ray_enc_train_dict(const uint64_t* v, int64_t n, ray_enc_kind_t k,
                           uint64_t** out, uint32_t* out_n);

/* ---- ALP (src/store/enc_alp.c, compiled with strict IEEE flags) ----
 * v holds T-bit float patterns (T 32 or 64), n in 1..1024. */
/* Runtime control check, not a change of the caller's FP environment.
 * Checks standard rounding, and SSE controls where available. Other control
 * modes must still obey the documented default-environment precondition. */
bool ray_enc_fp_env_ok(void);
#define RAY_ENC_FPENV_MSG "generation-2 float encoding/decoding requires default FP controls (FE_TONEAREST, masked exceptions, no FTZ/DAZ)"
#define RAY_ENC_ALP_CANDS 5
typedef struct { uint8_t e, f; } ray_enc_alp_ef_t;
typedef struct { ray_enc_alp_ef_t c[RAY_ENC_ALP_CANDS]; int n; } ray_enc_alp_cands_t;
/* Candidates from the filled bit patterns of a column's first chunk
 * (n values, T 32 or 64): every 32nd value of each 1024-value vector. */
void    ray_enc_alp_train(const uint64_t* bits, int64_t n, int T, ray_enc_alp_cands_t* out);
/* Smallest ALP body over the candidates (*best = its index), -1 when there
 * are none. */
int64_t ray_enc_alp_size(const uint64_t* v, int n, int T, const ray_enc_alp_cands_t* c, int* best);
bool    ray_enc_alp_write(const uint64_t* v, int n, int T, uint8_t e, uint8_t f, ray_enc_buf_t* out);
/* Validates and decodes an ALP body into T-bit patterns; returns the body
 * bytes or -1 (corrupt). */
int64_t ray_enc_alp_read(const uint8_t* p, size_t avail, int n, int T, uint64_t* out);

/* ---- ALP_RD (src/store/enc_alp.c): each value split into a left part
 * (the top T - right_bits bits, a code into an 8-entry dictionary, or an
 * exception) and its low right_bits bits packed.  v holds T-bit float
 * patterns (T 32 or 64), n in 1..1024.  The best right_bits in T-16..T-1
 * depends on v only, so size and write agree. ---- */
/* Smallest ALP_RD body, -1 on bad arguments or OOM. */
int64_t ray_enc_alprd_size(const uint64_t* v, int n, int T);
/* Appends the body; false on bad arguments (n outside 1..1024, T not 32
 * or 64) or OOM (out->oom), with no partial body left in out. */
bool    ray_enc_alprd_write(const uint64_t* v, int n, int T, ray_enc_buf_t* out);
/* Validates and decodes an ALP_RD body into T-bit patterns (out holds
 * RAY_ENC_VEC values); returns the body bytes or -1 (corrupt: out is
 * then untouched or all zero, never partly decoded). */
int64_t ray_enc_alprd_read(const uint8_t* p, size_t avail, int n, int T, uint64_t* out);

/* ---- the float top level ---- */
/* The float vector's top-level choice among RAW, CONST, RLE, DICT, ALP,
 * ALP_RD; `extra` is the trailer's bytes, counted on both sides of the
 * 1/8 gain rule.  dict and c may be NULL (no DICT, no ALP). */
ray_enc_choice_t ray_enc_float_choose(const uint64_t* v, int n, int T, const ray_enc_dict_t* dict,
                                      const ray_enc_alp_cands_t* c, int64_t extra);
/* Write `enc`'s body (false: not applicable, illegal or OOM; out->oom tells). */
bool    ray_enc_float_write(uint8_t enc, const uint64_t* v, int n, int T, const ray_enc_dict_t* dict,
                            const ray_enc_alp_cands_t* c, ray_enc_buf_t* out, ray_enc_choice_t* chosen);
/* Validate and decode any float body; bytes used or -1. */
int64_t ray_enc_float_read(const uint8_t* p, size_t avail, uint8_t enc, int n, int T,
                           const ray_enc_dict_t* dict, uint64_t* out);

/* ---- STR bodies (src/store/enc_str.c), n in 0..1024 ----
 * STR_DICT: child `codes` (U32), each below the STR_DICT entry count.
 * FSST: child `lengths` (U32, RAW or FOR_BP), u64 total_bytes (the sum of
 * the lengths), the compressed bytes padded to 8 with zeros.  Readers name
 * their reject rule through ray_enc_read_fail. */
int64_t ray_enc_strdict_size(const uint64_t* codes, int n);
bool    ray_enc_strdict_write(const uint64_t* codes, int n, ray_enc_buf_t* out);
/* codes (each below dict_n); the body must fill avail.  Bytes used, or -1. */
int64_t ray_enc_strdict_read(const uint8_t* p, size_t avail, int n, uint32_t dict_n, uint64_t* codes);
/* Compress n strings: buf receives the concatenated codes (capacity at
 * least 2 x the strings' bytes), clen[i] each string's compressed length.
 * Returns the total compressed bytes. */
uint64_t ray_enc_fsst_encode(const ray_fsst_table_t* t, const ray_fsst_index_t* ix,
                             const char* const* s, const uint32_t* len, int n,
                             uint8_t* buf, uint64_t* clen);
int64_t ray_enc_fsst_size(const uint64_t* clen, int n, uint64_t total);
/* Appends the body; false on bad arguments (n outside 0..1024, a length
 * above UINT32_MAX, total not their sum) or OOM (out->oom), with no
 * partial body left in out. */
bool    ray_enc_fsst_write(const uint64_t* clen, int n, const uint8_t* bytes, uint64_t total,
                           ray_enc_buf_t* out);
/* Validate an FSST body against table t (spec "FSST validation", vector
 * scope): every slice self-contained, codes below n_symbols or 255, each
 * expansion at most UINT32_MAX.  Fills clen[] and xlen[] (expanded
 * lengths) and *bytes (the compressed area).  The body must fill avail.
 * Body bytes, or -1. */
int64_t ray_enc_fsst_check(const uint8_t* p, size_t avail, int n, const ray_fsst_table_t* t,
                           uint64_t* clen, uint64_t* xlen, const uint8_t** bytes);

#endif /* RAY_ENC_H */
