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

#ifndef RAY_EMBMODEL_H
#define RAY_EMBMODEL_H

/*
 * embmodel.h -- a text-embedding model held by the engine: its tokenizer and
 * its weights (docs/docs/namespaces/emb.md).
 *
 * A model is opened from a directory as its publisher ships it (config.json,
 * tokenizer.json, model.safetensors) and never changes afterwards, so every
 * handle to it shares the one object by count.
 */
#include <rayforce.h>
#include <stdbool.h>

typedef struct ray_embmodel ray_embmodel_t;

/* Opens the model in `dir`.  NULL on failure, with the reason in *err (an
 * error object the caller returns). */
ray_embmodel_t* ray_embmodel_open(const char* dir, ray_t** err);
void ray_embmodel_retain(ray_embmodel_t* m);
void ray_embmodel_release(ray_embmodel_t* m);

/* What .emb.model.info gives. */
typedef struct {
    int64_t vocab, dim, layers, heads, inner, max_tokens;
    const char* kind;       /* the model's family, as its config names it */
    const char* tokenizer;  /* how it cuts text */
} ray_embmodel_info_t;
void ray_embmodel_info(const ray_embmodel_t* m, ray_embmodel_info_t* out);

/* What one worker needs while it cuts texts; made once, used for many. */
typedef struct ray_embwork ray_embwork_t;
ray_embwork_t* ray_embwork_new(const ray_embmodel_t* m);
void ray_embwork_free(ray_embwork_t* w);

/* The ids of `text`; *ids points into `w` and holds until its next use.
 * Returns how many, or -1 when memory runs out. */
int64_t ray_embmodel_tokens(const ray_embmodel_t* m, ray_embwork_t* w,
                            const char* text, size_t len, const int32_t** ids);

/* The vector of `text` in out[0 .. dim): the encoder's last layer averaged
 * over the tokens and scaled to length one.  A text longer than the model's
 * limit, or than max_tokens when that is positive and smaller, is cut to its
 * beginning.  With a pool the one text is computed by all its workers, which
 * the caller may ask for only where a dispatch is allowed
 * (ray_pool_par_dispatch_ok), and must then check for an interrupt before it
 * trusts the vector: a cancelled dispatch leaves it unfinished.  With NULL
 * it is computed by the calling thread, as a task of a dispatch must.
 * Returns the tokens used; -1 when memory runs out, -2 when the model was
 * opened without weights. */
struct ray_pool;
int64_t ray_embmodel_encode(const ray_embmodel_t* m, ray_embwork_t* w, struct ray_pool* pool,
                            const char* text, size_t len, int64_t max_tokens, float* out);

#endif /* RAY_EMBMODEL_H */
