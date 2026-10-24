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

#include "test.h"
#include <rayforce.h>
#include "mem/heap.h"
#include "table/sym.h"
#include "store/fsst.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fsst_setup(void)    { ray_heap_init(); (void)ray_sym_init(); }
static void fsst_teardown(void) { ray_sym_destroy(); ray_heap_destroy(); }

static int fsst_roundtrip_all(const ray_fsst_table_t* t, const uint8_t* const* s,
                              const uint32_t* l, int n, int64_t* total_enc) {
    *total_enc = 0;
    for (int i = 0; i < n; i++) {
        uint8_t* enc = malloc(2 * (size_t)l[i] + 1);
        uint8_t* dec = malloc((size_t)l[i] + 1);
        int64_t e = ray_fsst_encode(t, s[i], l[i], enc);
        int64_t x = ray_fsst_expanded_len(t, enc, (uint32_t)e);
        int64_t d = ray_fsst_decode(t, enc, (uint32_t)e, dec);
        int ok = e >= 0 && x == (int64_t)l[i] && d == (int64_t)l[i] && memcmp(dec, s[i], l[i]) == 0;
        *total_enc += e;
        free(enc); free(dec);
        if (!ok) return i;
    }
    return -1;
}

static test_result_t test_fsst_urls_compress(void) {
    enum { N = 2000 };
    static char buf[N][64];
    const uint8_t* s[N]; uint32_t l[N];
    int64_t raw = 0;
    for (int i = 0; i < N; i++) {
        int len = snprintf(buf[i], 64, "http://www.example.com/page/%d?ref=search", i * 7);
        s[i] = (const uint8_t*)buf[i]; l[i] = (uint32_t)len; raw += len;
    }
    ray_fsst_table_t t;
    ray_fsst_train(s, l, N, &t);
    TEST_ASSERT_TRUE(t.n > 0);
    int64_t enc;
    TEST_ASSERT_EQ_I(fsst_roundtrip_all(&t, s, l, N, &enc), -1);
    TEST_ASSERT_FMT(enc * 2 < raw, "enc %lld raw %lld", (long long)enc, (long long)raw);
    PASS();
}

static test_result_t test_fsst_all_bytes_empty_long(void) {
    static uint8_t all[256], longs[5000];
    for (int i = 0; i < 256; i++) all[i] = (uint8_t)i;
    for (int i = 0; i < 5000; i++) longs[i] = (uint8_t)("abcabcabd"[i % 9]);
    const uint8_t* s[3] = { all, (const uint8_t*)"", longs };
    uint32_t l[3] = { 256, 0, 5000 };
    ray_fsst_table_t t;
    ray_fsst_train(s, l, 3, &t);
    int64_t enc;
    TEST_ASSERT_EQ_I(fsst_roundtrip_all(&t, s, l, 3, &enc), -1);
    PASS();
}

static test_result_t test_fsst_deterministic(void) {
    const uint8_t* s[3] = { (const uint8_t*)"banana", (const uint8_t*)"bandana", (const uint8_t*)"cabana" };
    uint32_t l[3] = { 6, 7, 6 };
    ray_fsst_table_t a, b;
    ray_fsst_train(s, l, 3, &a);
    ray_fsst_train(s, l, 3, &b);
    TEST_ASSERT_TRUE(memcmp(&a, &b, sizeof a) == 0);
    TEST_ASSERT_EQ_I(ray_fsst_table_bytes(&a), 8 + ((a.n + 7) & ~7) + 8 * a.n);
    PASS();
}

static test_result_t test_fsst_encode_ix_matches_encode(void) {
    enum { N = 500 };
    static char buf[N][64];
    const uint8_t* s[N]; uint32_t l[N];
    for (int i = 0; i < N; i++) {
        int len = snprintf(buf[i], 64, "https://host%d.example.org/a/b/%d.html", i % 7, i * 13);
        s[i] = (const uint8_t*)buf[i]; l[i] = (uint32_t)len;
    }
    ray_fsst_table_t t; ray_fsst_train(s, l, N, &t);
    ray_fsst_index_t ix; ray_fsst_index_build(&t, &ix);
    for (int i = 0; i < N; i++) {
        uint8_t a[2 * 64 + 1], b[2 * 64 + 1];
        int64_t ea = ray_fsst_encode(&t, s[i], l[i], a);
        int64_t eb = ray_fsst_encode_ix(&t, &ix, s[i], l[i], b);
        TEST_ASSERT_EQ_I(ea, eb);
        TEST_ASSERT_TRUE(memcmp(a, b, (size_t)ea) == 0);
    }
    PASS();
}

static test_result_t test_fsst_train_covers_whole_input(void) {
    enum { N = 4000, H = 2000 };
    static char buf[N][160];
    const uint8_t* s[N]; uint32_t l[N];
    for (int i = 0; i < N; i++) {
        int len = i < H
            ? snprintf(buf[i], 160, "http://alpha.example.com/catalog/item/%d?lang=en&utm=0123456789012345678901234567890123456789012345", i * 3)
            : snprintf(buf[i], 160, "https://zeta.another-site.net/users/profile/%d/settings/notifications/email/digest/weekly/summary/archive", i * 5);
        s[i] = (const uint8_t*)buf[i]; l[i] = (uint32_t)len;
    }
    ray_fsst_table_t t; ray_fsst_train(s, l, N, &t);
    int64_t enc;
    TEST_ASSERT_EQ_I(fsst_roundtrip_all(&t, s, l, N, &enc), -1);
    int64_t raw2 = 0, enc2;
    for (int i = H; i < N; i++) raw2 += l[i];
    TEST_ASSERT_EQ_I(fsst_roundtrip_all(&t, s + H, l + H, N - H, &enc2), -1);
    TEST_ASSERT_FMT(enc2 * 3 < raw2 * 2, "last half enc %lld raw %lld", (long long)enc2, (long long)raw2);
    PASS();
}

const test_entry_t fsst_entries[] = {
    { "fsst/urls_compress",        test_fsst_urls_compress,        fsst_setup, fsst_teardown },
    { "fsst/all_bytes_empty_long", test_fsst_all_bytes_empty_long, fsst_setup, fsst_teardown },
    { "fsst/deterministic",        test_fsst_deterministic,        fsst_setup, fsst_teardown },
    { "fsst/encode_ix_matches_encode", test_fsst_encode_ix_matches_encode, fsst_setup, fsst_teardown },
    { "fsst/train_covers_whole_input", test_fsst_train_covers_whole_input, fsst_setup, fsst_teardown },
    { NULL, NULL, NULL, NULL },
};
