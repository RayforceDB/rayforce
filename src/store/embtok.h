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

#ifndef RAY_EMBTOK_H
#define RAY_EMBTOK_H

/* The tokenizer of a sentence-piece unigram model, as text encoders of the
 * XLM-R family use it: text to the ids the model was trained on.
 *
 * Four steps, each of them the reference tokenizer's, id for id:
 *
 *   normalize   the model's own table of replacements (a double-array trie
 *               over byte strings, with the replacement strings after it):
 *               a grapheme shorter than six bytes is looked up whole, a
 *               longer one character by character; the first key that is a
 *               prefix of the chunk gives the replacement of the whole chunk
 *   squeeze     a run of two or more spaces becomes one
 *   cut         every space becomes the mark U+2581, one is put in front,
 *               and the text is cut before each mark
 *   segment     each piece is cut into the vocabulary's tokens so that the
 *               sum of their scores is greatest (Viterbi); a character the
 *               vocabulary lacks costs the lowest score less ten, and
 *               neighbouring unknowns are one token
 *
 * The ids of a text are then <s>, the pieces' tokens, </s>.
 *
 * Nothing here knows the engine: memory comes through emb_alloc / emb_free,
 * which the file that includes this one defines. */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "store/embextend.h"

typedef struct {
    /* the table of replacements */
    const uint32_t* trie;     /* units of the double-array trie */
    uint32_t        trie_len;
    const char*     norm;     /* replacement strings, each ended by NUL */
    size_t          norm_len;
    void*           charsmap; /* owned copy that trie and norm point into */
    /* the vocabulary */
    char*     tok_bytes;      /* every token's bytes, one after another */
    uint32_t* tok_off;        /* n + 1 offsets into tok_bytes */
    double*   tok_score;
    uint32_t* slots;          /* open addressing: id + 1, or 0 for empty */
    uint32_t  slot_mask;
    int32_t   n;
    int32_t   unk, bos, eos;
    int32_t   max_tok;        /* the longest token, in bytes */
    double    unk_score;
} emb_tok_t;

/* What one text needs while it is cut; kept and grown between texts. */
typedef struct {
    char*    text;   size_t text_cap;    /* normalized, then marked */
    double*  best;   size_t dp_cap;      /* one entry for each byte of a piece, and one */
    int32_t* from;   size_t from_cap;
    int32_t* tok;    size_t tok_cap;
    int32_t* ids;    size_t ids_cap;
} emb_scratch_t;

static inline uint64_t emb_hash_step(uint64_t h, uint8_t c) { return (h ^ c) * 0x100000001b3ULL; }
#define EMB_HASH_SEED 0xcbf29ce484222325ULL

/* The id of the token with exactly these bytes, whose hash is h; -1 when there is none. */
static inline int32_t emb_find(const emb_tok_t* t, const char* p, uint32_t len, uint64_t h) {
    for (uint32_t s = (uint32_t)(h ^ (h >> 32)) & t->slot_mask;; s = (s + 1) & t->slot_mask) {
        uint32_t v = t->slots[s];
        if (v == 0) return -1;
        uint32_t id = v - 1, o = t->tok_off[id];
        if (t->tok_off[id + 1] - o == len && memcmp(t->tok_bytes + o, p, len) == 0) return (int32_t)id;
    }
}

static void emb_tok_free(emb_tok_t* t) {
    if (!t) return;
    emb_free(t->charsmap); emb_free(t->tok_bytes); emb_free(t->tok_off);
    emb_free(t->tok_score); emb_free(t->slots); emb_free(t);
}

/* A tokenizer from the table of replacements as the model's file holds it
 * decoded (a 32-bit length of the trie in bytes, the trie, the strings) and
 * the vocabulary in id order.  NULL when memory runs out or the table is
 * not in form. */
static emb_tok_t* emb_tok_new(const uint8_t* charsmap, size_t charsmap_len,
                              const char* const* toks, const uint32_t* lens, const double* scores,
                              int32_t n, int32_t unk, int32_t bos, int32_t eos) {
    if (n <= 0 || unk < 0 || unk >= n || bos < 0 || bos >= n || eos < 0 || eos >= n) return NULL;
    emb_tok_t* t = (emb_tok_t*)emb_alloc(sizeof *t);
    if (!t) return NULL;
    memset(t, 0, sizeof *t);
    t->n = n; t->unk = unk; t->bos = bos; t->eos = eos;
    if (charsmap_len >= 4) {
        uint32_t tsize;
        memcpy(&tsize, charsmap, 4);
        if (tsize % 4 != 0 || (size_t)tsize + 4 > charsmap_len) { emb_tok_free(t); return NULL; }
        t->charsmap = emb_alloc(charsmap_len);
        if (!t->charsmap) { emb_tok_free(t); return NULL; }
        memcpy(t->charsmap, charsmap, charsmap_len);
        t->trie = (const uint32_t*)((const char*)t->charsmap + 4);
        t->trie_len = tsize / 4;
        t->norm = (const char*)t->charsmap + 4 + tsize;
        t->norm_len = charsmap_len - 4 - tsize;
    }
    size_t total = 0;
    for (int32_t i = 0; i < n; i++) total += lens[i];
    uint32_t cap = 16;
    while (cap < (uint32_t)n * 2u) cap <<= 1;
    t->tok_bytes = (char*)emb_alloc(total + 1);
    t->tok_off   = (uint32_t*)emb_alloc(((size_t)n + 1) * sizeof(uint32_t));
    t->tok_score = (double*)emb_alloc((size_t)n * sizeof(double));
    t->slots     = (uint32_t*)emb_alloc((size_t)cap * sizeof(uint32_t));
    if (!t->tok_bytes || !t->tok_off || !t->tok_score || !t->slots) { emb_tok_free(t); return NULL; }
    memset(t->slots, 0, (size_t)cap * sizeof(uint32_t));
    t->slot_mask = cap - 1;
    double low = 0.0;
    uint32_t o = 0;
    for (int32_t i = 0; i < n; i++) {
        t->tok_off[i] = o;
        memcpy(t->tok_bytes + o, toks[i], lens[i]);
        o += lens[i];
        t->tok_score[i] = scores[i];
        if (i == 0 || scores[i] < low) low = scores[i];
        if ((int32_t)lens[i] > t->max_tok) t->max_tok = (int32_t)lens[i];
    }
    t->tok_off[n] = o;
    t->unk_score = low - 10.0;
    for (int32_t i = 0; i < n; i++) {
        uint64_t h = EMB_HASH_SEED;
        const char* p = t->tok_bytes + t->tok_off[i];
        for (uint32_t k = 0; k < lens[i]; k++) h = emb_hash_step(h, (uint8_t)p[k]);
        /* Of two tokens with the same bytes the first keeps the name. */
        if (emb_find(t, p, lens[i], h) >= 0) continue;
        uint32_t s = (uint32_t)(h ^ (h >> 32)) & t->slot_mask;
        while (t->slots[s] != 0) s = (s + 1) & t->slot_mask;
        t->slots[s] = (uint32_t)i + 1;
    }
    return t;
}

static void emb_scratch_free(emb_scratch_t* s) {
    emb_free(s->text); emb_free(s->best); emb_free(s->from); emb_free(s->tok); emb_free(s->ids);
    memset(s, 0, sizeof *s);
}

static bool emb_grow(void** p, size_t* cap, size_t want, size_t esz, size_t keep) {
    if (want <= *cap) return true;
    size_t c = *cap ? *cap : 256;
    while (c < want) c *= 2;
    void* q = emb_alloc(c * esz);
    if (!q) return false;
    if (*p && keep) memcpy(q, *p, keep * esz);
    emb_free(*p);
    *p = q; *cap = c;
    return true;
}

/* The length of the character that begins at p (n bytes left) and its code
 * point.  Bytes that are no UTF-8 are a character each, so that any input
 * is cut somehow and nothing reads past its end. */
static inline uint32_t emb_char(const uint8_t* p, size_t n, uint32_t* cp) {
    uint8_t c = p[0];
    if (c < 0x80) { *cp = c; return 1; }
    uint32_t len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    if (len == 1 || len > n) { *cp = 0xFFFD; return 1; }
    uint32_t v = c & (0xFFu >> (len + 1));
    for (uint32_t i = 1; i < len; i++) {
        if ((p[i] & 0xC0) != 0x80) { *cp = 0xFFFD; return 1; }
        v = (v << 6) | (p[i] & 0x3F);
    }
    *cp = v;
    return len;
}

static inline bool emb_clings(uint32_t cp) {
    if (cp < 0x0300) return false;
    size_t lo = 0, hi = sizeof emb_extend / sizeof emb_extend[0];
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (cp > emb_extend[mid][1]) lo = mid + 1;
        else if (cp < emb_extend[mid][0]) hi = mid;
        else return true;
    }
    return false;
}

/* The replacement of the chunk p[0..n): the string of the first key of the
 * table that is a prefix of it, or NULL when no key is. */
static inline const char* emb_replace(const emb_tok_t* t, const uint8_t* p, size_t n) {
    if (t->trie_len == 0) return NULL;
    const uint32_t* a = t->trie;
    uint32_t pos = 0, u = a[0];
    pos ^= (u >> 10) << ((u & 0x200u) >> 6);
    for (size_t i = 0; i < n; i++) {
        uint8_t c = p[i];
        if (c == 0) return NULL;
        pos ^= c;
        if (pos >= t->trie_len) return NULL;
        u = a[pos];
        if ((u & 0x800000FFu) != c) return NULL;
        pos ^= (u >> 10) << ((u & 0x200u) >> 6);
        if (pos >= t->trie_len) return NULL;
        if ((u >> 8) & 1) {
            uint32_t at = a[pos] & 0x7FFFFFFFu;
            return at < t->norm_len ? t->norm + at : NULL;
        }
    }
    return NULL;
}

/* Normalizes, squeezes and marks `in` into s->text; returns its length, or
 * -1 when memory runs out. */
static int64_t emb_prepare(const emb_tok_t* t, emb_scratch_t* s, const uint8_t* in, size_t n) {
    size_t len = 0;
#define EMB_PUT(src, k) do { \
        size_t k_ = (k); \
        if (!emb_grow((void**)&s->text, &s->text_cap, len + k_ + 8, 1, len)) return -1; \
        memcpy(s->text + len, (src), k_); len += k_; \
    } while (0)
    for (size_t i = 0; i < n;) {
        uint32_t cp, first = emb_char(in + i, n - i, &cp);
        size_t g = first;
        if (cp == '\r' && i + g < n && in[i + g] == '\n') g += 1;
        else for (;;) {
            if (i + g >= n) break;
            uint32_t next, k = emb_char(in + i + g, n - i - g, &next);
            if (!emb_clings(next)) break;
            g += k;
        }
        const char* r = g < 6 ? emb_replace(t, in + i, g) : NULL;
        if (r) EMB_PUT(r, strlen(r));
        else for (size_t j = 0; j < g;) {
            uint32_t c2, k = emb_char(in + i + j, g - j, &c2);
            const char* r1 = emb_replace(t, in + i + j, k);
            if (r1) EMB_PUT(r1, strlen(r1));
            else EMB_PUT(in + i + j, k);
            j += k;
        }
        i += g;
    }
#undef EMB_PUT
    if (len == 0) return 0;
    /* A run of spaces is one space; then every space is the mark, and one
     * stands in front unless the text begins with one. */
    size_t w = 0;
    for (size_t i = 0; i < len; i++) {
        if (s->text[i] == ' ' && w > 0 && s->text[w - 1] == ' ') continue;
        s->text[w++] = s->text[i];
    }
    len = w;
    size_t spaces = 0;
    for (size_t i = 0; i < len; i++) spaces += s->text[i] == ' ';
    bool front = s->text[0] != ' ';
    size_t out = len + spaces * 2 + (front ? 3 : 0);
    if (!emb_grow((void**)&s->text, &s->text_cap, out + 8, 1, len)) return -1;
    size_t r = len, q = out;
    while (r > 0) {
        char c = s->text[--r];
        if (c == ' ') { q -= 3; memcpy(s->text + q, "\xE2\x96\x81", 3); }
        else s->text[--q] = c;
    }
    if (front) memcpy(s->text, "\xE2\x96\x81", 3);
    return (int64_t)out;
}

static inline bool emb_is_mark(const char* p, size_t n) {
    return n >= 3 && (uint8_t)p[0] == 0xE2 && (uint8_t)p[1] == 0x96 && (uint8_t)p[2] == 0x81;
}

/* Cuts the piece p[0..n) into tokens and appends their ids to s->ids at
 * *count.  False when memory runs out. */
static bool emb_segment(const emb_tok_t* t, emb_scratch_t* s, const char* p, size_t n, size_t* count) {
    if (!emb_grow((void**)&s->best, &s->dp_cap, n + 1, sizeof(double), 0)) return false;
    if (!emb_grow((void**)&s->from, &s->from_cap, n + 1, sizeof(int32_t), 0)) return false;
    if (!emb_grow((void**)&s->tok, &s->tok_cap, n + 1, sizeof(int32_t), 0)) return false;
    double* best = s->best; int32_t* from = s->from; int32_t* tok = s->tok;
    for (size_t i = 0; i <= n; i++) { best[i] = -1e300; from[i] = -1; tok[i] = -1; }
    best[0] = 0.0;
    for (size_t i = 0; i < n;) {
        uint32_t cp, clen = emb_char((const uint8_t*)p + i, n - i, &cp);
        if (best[i] > -1e299) {
            bool one = false;
            uint64_t h = EMB_HASH_SEED;
            size_t lim = n - i < (size_t)t->max_tok ? n - i : (size_t)t->max_tok;
            for (size_t j = 0; j < lim;) {
                uint32_t c2, k = emb_char((const uint8_t*)p + i + j, n - i - j, &c2);
                if (j + k > lim) break;
                for (uint32_t b = 0; b < k; b++) h = emb_hash_step(h, (uint8_t)p[i + j + b]);
                j += k;
                int32_t id = emb_find(t, p + i, (uint32_t)j, h);
                if (id < 0) continue;
                if (j == clen) one = true;
                double sc = best[i] + t->tok_score[id];
                if (sc > best[i + j]) { best[i + j] = sc; from[i + j] = (int32_t)i; tok[i + j] = id; }
            }
            if (!one) {
                double sc = best[i] + t->unk_score;
                if (sc > best[i + clen]) { best[i + clen] = sc; from[i + clen] = (int32_t)i; tok[i + clen] = t->unk; }
            }
        }
        i += clen;
    }
    /* Read the cut backwards, then turn it round where it lies. */
    size_t start = *count, k = start;
    for (int64_t at = (int64_t)n; at > 0; at = from[at]) {
        if (!emb_grow((void**)&s->ids, &s->ids_cap, k + 2, sizeof(int32_t), k)) return false;
        s->ids[k++] = tok[at];
    }
    for (size_t a = start, b = k; a + 1 < b; a++, b--) { int32_t x = s->ids[a]; s->ids[a] = s->ids[b - 1]; s->ids[b - 1] = x; }
    size_t w = start;
    for (size_t r = start; r < k; r++) {
        if (s->ids[r] == t->unk && w > start && s->ids[w - 1] == t->unk) continue;
        s->ids[w++] = s->ids[r];
    }
    *count = w;
    return true;
}

/* The ids of `text`, in s->ids; returns how many, or -1 when memory runs out. */
static int64_t emb_tok_encode(const emb_tok_t* t, emb_scratch_t* s, const char* text, size_t n) {
    int64_t len = emb_prepare(t, s, (const uint8_t*)text, n);
    if (len < 0) return -1;
    size_t count = 0;
    if (!emb_grow((void**)&s->ids, &s->ids_cap, 4, sizeof(int32_t), 0)) return -1;
    s->ids[count++] = t->bos;
    const char* p = s->text;
    for (size_t i = 0; i < (size_t)len;) {
        size_t j = i + (emb_is_mark(p + i, (size_t)len - i) ? 3 : 0);
        while (j < (size_t)len && !emb_is_mark(p + j, (size_t)len - j)) j++;
        if (!emb_segment(t, s, p + i, j - i, &count)) return -1;
        i = j;
    }
    if (!emb_grow((void**)&s->ids, &s->ids_cap, count + 2, sizeof(int32_t), count)) return -1;
    s->ids[count++] = t->eos;
    return (int64_t)count;
}

#endif /* RAY_EMBTOK_H */
