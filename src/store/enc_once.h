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
#ifndef RAY_ENC_ONCE_H
#define RAY_ENC_ONCE_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <rayforce.h>

/* Attempt result / error envelope (spec "Error envelopes"): fixed size, so
 * recording it needs no allocation and cannot fail on OOM. */
typedef struct {
    char code[8];
    char msg[256];
    bool is_transient;
} ray_enc_snap_t;

enum { RAY_ENC_ONCE_IDLE = 0, RAY_ENC_ONCE_RUNNING, RAY_ENC_ONCE_DONE,
       RAY_ENC_ONCE_FAILED, RAY_ENC_ONCE_DRAINING };

/* One once-state with its attempt record inline (spec "Attempt records"):
 * a transient failure resets to IDLE only when the attempt's last waiter
 * has copied the snapshot; arrivals in between wait (DRAINING). */
typedef struct {
    _Atomic(uint32_t) lock;
    _Atomic(uint32_t) state;
    uint32_t waiters;
    uint64_t attempt;
    ray_enc_snap_t snap;
} ray_enc_once_t;

typedef ray_t* (*ray_enc_once_fn)(void* ctx, ray_enc_snap_t* fail);

void     ray_enc_once_init(ray_enc_once_t* o);
ray_t*   ray_enc_once_get(ray_enc_once_t* o, ray_t** slot, ray_enc_once_fn fn, void* ctx);
uint32_t ray_enc_once_waiters(ray_enc_once_t* o);
bool     ray_enc_code_transient(const char* code);
void     ray_enc_snap_take(ray_enc_snap_t* s, ray_t* err);
void     ray_enc_snap_set(ray_enc_snap_t* s, const char* code, const char* fmt, ...);
ray_t*   ray_enc_snap_error(const ray_enc_snap_t* s);
void     ray_enc_yield(void);

#endif /* RAY_ENC_ONCE_H */
