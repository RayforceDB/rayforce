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

#include "core/tz.h"
#include "mem/heap.h"

#include <string.h>

static void tz_setup(void)    { ray_heap_init(); }
static void tz_teardown(void) { ray_heap_destroy(); }

/* ---- A TZif file built in memory ------------------------------------------ */

static uint8_t* put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
    return p + 4;
}

static uint8_t* put_header(uint8_t* p, char version, uint32_t leap, uint32_t ntime, uint32_t ntype) {
    memset(p, 0, 44);
    memcpy(p, "TZif", 4);
    p[4] = (uint8_t)version;
    put32(p + 28, leap);
    put32(p + 32, ntime);
    put32(p + 36, ntype);
    put32(p + 40, 1);   /* one abbreviation byte */
    return p + 44;
}

/* Version `version`: an empty 32-bit block, then `ntime` changes at[] (Unix
 * seconds) into the types idx[], whose offsets are utoff[], then the footer. */
static size_t tzif(uint8_t* out, char version, uint32_t leap, const int64_t* at, const uint8_t* idx,
                   uint32_t ntime, const int32_t* utoff, uint32_t ntype, const char* footer) {
    uint8_t* p = put_header(out, version, 0, 0, 1);
    memset(p, 0, 7);   /* one type, one abbreviation byte */
    p = put_header(p + 7, version, leap, ntime, ntype);
    for (uint32_t i = 0; i < ntime; i++) {
        p = put32(p, (uint32_t)((uint64_t)at[i] >> 32));
        p = put32(p, (uint32_t)(uint64_t)at[i]);
    }
    for (uint32_t i = 0; i < ntime; i++) *p++ = idx[i];
    for (uint32_t i = 0; i < ntype; i++) {
        p = put32(p, (uint32_t)utoff[i]);
        *p++ = 0;
        *p++ = 0;
    }
    *p++ = 0;
    memset(p, 0, (size_t)leap * 12);
    p += (size_t)leap * 12;
    if (footer) {
        size_t n = strlen(footer);
        *p++ = '\n';
        memcpy(p, footer, n);
        p += n;
        *p++ = '\n';
    }
    return (size_t)(p - out);
}

#define U2000 946684800LL

/* The zone's offset in seconds at `sec` seconds from 2000 plus `ns`. */
static int64_t off_at(const ray_tz_t* z, int64_t sec, int64_t ns) {
    return z->off[ray_tz_find(z, sec * 1000000000LL + ns)] / 1000000000LL;
}

/* New York: local mean time, then the two changes of 2006, then the rule. */
static ray_tz_t* new_york(void) {
    static const int64_t at[] = {1143961200, 1162101600};   /* 2006.04.02D07, 2006.10.29D06 */
    static const uint8_t idx[] = {1, 2};
    static const int32_t utoff[] = {-17762, -14400, -18000};
    uint8_t buf[256];
    size_t n = tzif(buf, '2', 0, at, idx, 2, utoff, 3, "EST5EDT,M3.2.0,M11.1.0");
    return ray_tz_parse(buf, n);
}

/* ---- Tests ----------------------------------------------------------------- */

static test_result_t test_tz_name(void) {
    TEST_ASSERT_TRUE(ray_tz_name_ok("UTC", 3));
    TEST_ASSERT_TRUE(ray_tz_name_ok("America/New_York", 16));
    TEST_ASSERT_TRUE(ray_tz_name_ok("America/Argentina/Buenos_Aires", 30));
    TEST_ASSERT_TRUE(ray_tz_name_ok("Etc/GMT+5", 9));
    TEST_ASSERT_TRUE(ray_tz_name_ok("Etc/GMT-14", 10));
    TEST_ASSERT_FALSE(ray_tz_name_ok("", 0));
    TEST_ASSERT_FALSE(ray_tz_name_ok("/etc/passwd", 11));
    TEST_ASSERT_FALSE(ray_tz_name_ok("../UTC", 6));
    TEST_ASSERT_FALSE(ray_tz_name_ok("America/../UTC", 14));
    TEST_ASSERT_FALSE(ray_tz_name_ok("America/", 8));
    TEST_ASSERT_FALSE(ray_tz_name_ok("America//New_York", 17));
    TEST_ASSERT_FALSE(ray_tz_name_ok("zone.tab", 8));
    TEST_ASSERT_FALSE(ray_tz_name_ok("A B", 3));
    TEST_ASSERT_FALSE(ray_tz_name_ok("A\\B", 3));
    TEST_ASSERT_FALSE(ray_tz_name_ok("UTC\0x", 5));
    char longname[80];
    memset(longname, 'a', sizeof(longname));
    TEST_ASSERT_FALSE(ray_tz_name_ok(longname, sizeof(longname)));
    PASS();
}

static test_result_t test_tz_listed_changes(void) {
    ray_tz_t* z = new_york();
    TEST_ASSERT_NOT_NULL(z);
    /* before the first change: type 0, local mean time */
    TEST_ASSERT_EQ_I(off_at(z, -3155673600LL, 0), -17762);          /* 1900.01.01 */
    TEST_ASSERT_EQ_I(off_at(z, 1143961200 - U2000, -1), -17762);
    TEST_ASSERT_EQ_I(off_at(z, 1143961200 - U2000, 0), -14400);     /* a change holds from its instant */
    TEST_ASSERT_EQ_I(off_at(z, 1162101600 - U2000, -1), -14400);
    TEST_ASSERT_EQ_I(off_at(z, 1162101600 - U2000, 0), -18000);
    ray_tz_free(z);
    PASS();
}

static test_result_t test_tz_rule_unrolled(void) {
    ray_tz_t* z = new_york();
    TEST_ASSERT_NOT_NULL(z);
    TEST_ASSERT_EQ_I(off_at(z, 757382400, 0), -18000);    /* 2024.01.01 */
    TEST_ASSERT_EQ_I(off_at(z, 763369200, -1), -18000);   /* 2024.03.10D07:00, second Sunday of March */
    TEST_ASSERT_EQ_I(off_at(z, 763369200, 0), -14400);
    TEST_ASSERT_EQ_I(off_at(z, 773107200, 0), -14400);    /* 2024.07.01 */
    TEST_ASSERT_EQ_I(off_at(z, 783928800, -1), -14400);   /* 2024.11.03D06:00, first Sunday of November */
    TEST_ASSERT_EQ_I(off_at(z, 783928800, 0), -18000);
    /* 2100 is not a leap year */
    TEST_ASSERT_EQ_I(off_at(z, 3162006000LL, -1), -18000);   /* 2100.03.14D07:00 */
    TEST_ASSERT_EQ_I(off_at(z, 3162006000LL, 0), -14400);
    TEST_ASSERT_EQ_I(off_at(z, 3182565600LL, -1), -14400);   /* 2100.11.07D06:00 */
    TEST_ASSERT_EQ_I(off_at(z, 3182565600LL, 0), -18000);
    /* the last TIMESTAMP is still inside the table */
    int64_t last = off_at(z, 9223372036LL, 0);
    TEST_ASSERT_TRUE(last == -18000 || last == -14400);
    for (int32_t i = 1; i < z->n; i++) TEST_ASSERT_TRUE(z->at[i - 1] < z->at[i]);
    ray_tz_free(z);
    PASS();
}

/* Sydney's rule and no listed change: DST runs over the new year, ends at
 * 03:00 on DST, and the offset is east of UTC. */
static test_result_t test_tz_southern_rule(void) {
    static const int32_t utoff[] = {36000};
    uint8_t buf[256];
    size_t n = tzif(buf, '3', 0, NULL, NULL, 0, utoff, 1, "AEST-10AEDT,M10.1.0,M4.1.0/3");
    ray_tz_t* z = ray_tz_parse(buf, n);
    TEST_ASSERT_NOT_NULL(z);
    TEST_ASSERT_EQ_I(off_at(z, 757382400, 0), 39600);    /* 2024.01.01 */
    TEST_ASSERT_EQ_I(off_at(z, 765734400, -1), 39600);   /* 2024.04.06D16:00 */
    TEST_ASSERT_EQ_I(off_at(z, 765734400, 0), 36000);
    TEST_ASSERT_EQ_I(off_at(z, 773107200, 0), 36000);    /* 2024.07.01 */
    TEST_ASSERT_EQ_I(off_at(z, 781459200, -1), 36000);   /* 2024.10.05D16:00 */
    TEST_ASSERT_EQ_I(off_at(z, 781459200, 0), 39600);
    for (int32_t i = 1; i < z->n; i++) TEST_ASSERT_TRUE(z->at[i - 1] < z->at[i]);
    ray_tz_free(z);
    PASS();
}

static test_result_t test_tz_fixed_offset(void) {
    static const int32_t utoff[] = {19800};
    uint8_t buf[256];
    size_t n = tzif(buf, '2', 0, NULL, NULL, 0, utoff, 1, "IST-5:30");
    ray_tz_t* z = ray_tz_parse(buf, n);
    TEST_ASSERT_NOT_NULL(z);
    TEST_ASSERT_EQ_I(z->n, 0);
    TEST_ASSERT_EQ_I(off_at(z, 757382400, 0), 19800);
    ray_tz_free(z);

    /* a quoted name, and no footer at all */
    n = tzif(buf, '2', 0, NULL, NULL, 0, utoff, 1, "<+0530>-5:30");
    z = ray_tz_parse(buf, n);
    TEST_ASSERT_NOT_NULL(z);
    ray_tz_free(z);
    n = tzif(buf, '2', 0, NULL, NULL, 0, utoff, 1, "");
    z = ray_tz_parse(buf, n);
    TEST_ASSERT_NOT_NULL(z);
    TEST_ASSERT_EQ_I(off_at(z, 0, 0), 19800);
    ray_tz_free(z);
    PASS();
}

/* `Jn` never counts 29 February and `n` does: in 2024 J60 is 1 March and
 * day 300 is 300 days after 1 January. */
static test_result_t test_tz_julian_rules(void) {
    static const int32_t utoff[] = {0};
    uint8_t buf[256];
    size_t n = tzif(buf, '2', 0, NULL, NULL, 0, utoff, 1, "XXX0YYY,J60,300/1");
    ray_tz_t* z = ray_tz_parse(buf, n);
    TEST_ASSERT_NOT_NULL(z);
    TEST_ASSERT_EQ_I(off_at(z, 762573600, -1), 0);      /* 2024.03.01D02:00 */
    TEST_ASSERT_EQ_I(off_at(z, 762573600, 0), 3600);
    TEST_ASSERT_EQ_I(off_at(z, 783302400, -1), 3600);   /* day 300 at 01:00 on DST */
    TEST_ASSERT_EQ_I(off_at(z, 783302400, 0), 0);
    ray_tz_free(z);
    PASS();
}

/* A change outside the TIMESTAMP range is folded into what holds before the
 * table, or dropped past its end. */
static test_result_t test_tz_out_of_range_changes(void) {
    static const int64_t at[] = {INT64_MIN, 1143961200, INT64_MAX};
    static const uint8_t idx[] = {1, 2, 0};
    static const int32_t utoff[] = {100, 200, 300};
    uint8_t buf[256];
    size_t n = tzif(buf, '2', 0, at, idx, 3, utoff, 3, "");
    ray_tz_t* z = ray_tz_parse(buf, n);
    TEST_ASSERT_NOT_NULL(z);
    TEST_ASSERT_EQ_I(z->n, 1);
    TEST_ASSERT_EQ_I(off_at(z, 0, 0), 200);
    TEST_ASSERT_EQ_I(off_at(z, 757382400, 0), 300);
    ray_tz_free(z);
    PASS();
}

static test_result_t test_tz_malformed(void) {
    static const int64_t at[] = {1143961200, 1162101600};
    static const uint8_t idx[] = {1, 2};
    static const int32_t utoff[] = {-17762, -14400, -18000};
    uint8_t buf[256];
    size_t n = tzif(buf, '2', 0, at, idx, 2, utoff, 3, "EST5EDT,M3.2.0,M11.1.0");
    size_t footer = n - strlen("\nEST5EDT,M3.2.0,M11.1.0\n");

    /* every truncation: cut inside the blocks or inside the footer */
    for (size_t k = 0; k < n; k++) {
        ray_tz_t* z = ray_tz_parse(buf, k);
        if (k != footer) TEST_ASSERT_FMT(z == NULL, "a file cut at %zu of %zu parsed", k, n);
        ray_tz_free(z);
    }
    TEST_ASSERT_NULL(ray_tz_parse(NULL, 0));

    uint8_t bad[256];
    memcpy(bad, buf, n); bad[0] = 'X';
    TEST_ASSERT_NULL(ray_tz_parse(bad, n));                 /* magic */
    n = tzif(bad, '1', 0, at, idx, 2, utoff, 3, "EST5EDT,M3.2.0,M11.1.0");
    TEST_ASSERT_NULL(ray_tz_parse(bad, n));                 /* no 64-bit block */
    n = tzif(bad, '\0', 0, at, idx, 2, utoff, 3, "EST5EDT,M3.2.0,M11.1.0");
    TEST_ASSERT_NULL(ray_tz_parse(bad, n));
    n = tzif(bad, '2', 1, at, idx, 2, utoff, 3, "EST5EDT,M3.2.0,M11.1.0");
    TEST_ASSERT_NULL(ray_tz_parse(bad, n));                 /* leap seconds */

    static const uint8_t idx_out[] = {1, 3};
    n = tzif(bad, '2', 0, at, idx_out, 2, utoff, 3, "");
    TEST_ASSERT_NULL(ray_tz_parse(bad, n));                 /* a type the file lacks */
    static const int64_t at_back[] = {1162101600, 1143961200};
    n = tzif(bad, '2', 0, at_back, idx, 2, utoff, 3, "");
    TEST_ASSERT_NULL(ray_tz_parse(bad, n));                 /* changes out of order */
    static const int32_t utoff_far[] = {-17762, 999999, -18000};
    n = tzif(bad, '2', 0, at, idx, 2, utoff_far, 3, "");
    TEST_ASSERT_NULL(ray_tz_parse(bad, n));                 /* an offset of eleven days */

    static const char* footers[] = {
        "EST5EDT",                   /* DST with no rule */
        "EST5EDT,M3.2.0",            /* one rule */
        "EST5EDT,M13.2.0,M11.1.0",   /* month 13 */
        "EST5EDT,M3.6.0,M11.1.0",    /* week 6 */
        "EST5EDT,M3.2.7,M11.1.0",    /* weekday 7 */
        "EST5EDT,J0,J365",           /* J counts from 1 */
        "EST5EDT,M3.2.0,M11.1.0x",   /* trailing byte */
        "ES5",                       /* a two-letter name */
        "EST",                       /* no offset */
        "EST25",                     /* 25 hours */
        "<+05",                      /* an open quote */
        "EST5EDT,M3.2.0/200,M11.1.0" /* a time past 167 hours */
    };
    for (size_t i = 0; i < sizeof(footers) / sizeof(footers[0]); i++) {
        n = tzif(bad, '2', 0, at, idx, 2, utoff, 3, footers[i]);
        TEST_ASSERT_FMT(ray_tz_parse(bad, n) == NULL, "footer \"%s\" parsed", footers[i]);
    }
    PASS();
}

/* ---- The database built in ------------------------------------------------- */

static ray_tz_t* builtin(const char* name) {
    size_t n = 0;
    const uint8_t* p = ray_tzdata_find(name, strlen(name), &n);
    return p ? ray_tz_parse(p, n) : NULL;
}

/* Every zone of the index is found by its name, parses, and the index is in
 * the order the lookup searches. */
static test_result_t test_tzdata_every_zone_parses(void) {
    TEST_ASSERT_TRUE(strlen(ray_tzdata_version) >= 5);
    TEST_ASSERT_TRUE(ray_tzdata_count > 300);
    for (int32_t i = 0; i < ray_tzdata_count; i++) {
        const char* name = ray_tzdata_zones[i].name;
        if (i) TEST_ASSERT_FMT(strcmp(ray_tzdata_zones[i - 1].name, name) < 0, "%s is out of order", name);
        size_t n = 0;
        const uint8_t* p = ray_tzdata_find(name, strlen(name), &n);
        TEST_ASSERT_FMT(p == ray_tzdata_blob + ray_tzdata_zones[i].at && n == ray_tzdata_zones[i].len,
                        "%s is not found by its name", name);
        ray_tz_t* z = ray_tz_parse(p, n);
        TEST_ASSERT_FMT(z != NULL, "%s does not parse", name);
        for (int32_t k = 1; k < z->n; k++)
            TEST_ASSERT_FMT(z->at[k - 1] < z->at[k], "%s: changes out of order", name);
        ray_tz_free(z);
    }
    PASS();
}

static test_result_t test_tzdata_lookup(void) {
    size_t n = 0, m = 0;
    TEST_ASSERT_NULL(ray_tzdata_find("Mars/Olympus_Mons", 17, &n));
    TEST_ASSERT_NULL(ray_tzdata_find("America/New_Yor", 15, &n));
    TEST_ASSERT_NULL(ray_tzdata_find("America/New_York1", 17, &n));
    TEST_ASSERT_NULL(ray_tzdata_find("../etc/passwd", 13, &n));
    TEST_ASSERT_NULL(ray_tzdata_find("", 0, &n));
    /* an old name is the same file as the zone it links to */
    const uint8_t* a = ray_tzdata_find("America/New_York", 16, &n);
    const uint8_t* b = ray_tzdata_find("US/Eastern", 10, &m);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_TRUE(a == b && n == m);
    PASS();
}

/* Offsets no release of the database has changed, read from the real zones. */
static test_result_t test_tzdata_known_offsets(void) {
    ray_tz_t* z = builtin("America/New_York");
    TEST_ASSERT_NOT_NULL(z);
    TEST_ASSERT_EQ_I(off_at(z, -4733510400LL, 0), -17762);   /* 1850.01.01, local mean time */
    TEST_ASSERT_EQ_I(off_at(z, -3155673600LL, 0), -18000);   /* 1900.01.01, standard time since 1883 */
    TEST_ASSERT_EQ_I(off_at(z, 763369200, -1), -18000);      /* 2024.03.10D07:00 */
    TEST_ASSERT_EQ_I(off_at(z, 763369200, 0), -14400);
    TEST_ASSERT_EQ_I(off_at(z, 783928800, -1), -14400);      /* 2024.11.03D06:00 */
    TEST_ASSERT_EQ_I(off_at(z, 783928800, 0), -18000);
    ray_tz_free(z);

    static const struct { const char* name; int64_t jan, jul; } zones[] = {   /* 2024.01.01, 2024.07.01 */
        { "UTC",                 0,     0     },
        { "Etc/GMT+5",           -18000, -18000 },
        { "Asia/Kolkata",        19800, 19800 },
        { "Asia/Kathmandu",      20700, 20700 },   /* a quarter-hour zone */
        { "Europe/London",       0,     3600  },
        { "Europe/Dublin",       0,     3600  },   /* its rule has winter as the DST */
        { "Australia/Sydney",    39600, 36000 },
        { "Australia/Lord_Howe", 39600, 37800 },   /* DST of half an hour */
        { "America/Sao_Paulo",   -10800, -10800 },
    };
    for (size_t i = 0; i < sizeof(zones) / sizeof(zones[0]); i++) {
        z = builtin(zones[i].name);
        TEST_ASSERT_FMT(z != NULL, "%s is not built in", zones[i].name);
        TEST_ASSERT_FMT(off_at(z, 757382400, 0) == zones[i].jan, "%s in January: %lld", zones[i].name,
                        (long long)off_at(z, 757382400, 0));
        TEST_ASSERT_FMT(off_at(z, 773107200, 0) == zones[i].jul, "%s in July: %lld", zones[i].name,
                        (long long)off_at(z, 773107200, 0));
        ray_tz_free(z);
    }
    PASS();
}

const test_entry_t tz_entries[] = {
    { "tz/name",                 test_tz_name,                 NULL,     NULL },
    { "tz/listed_changes",       test_tz_listed_changes,       tz_setup, tz_teardown },
    { "tz/rule_unrolled",        test_tz_rule_unrolled,        tz_setup, tz_teardown },
    { "tz/southern_rule",        test_tz_southern_rule,        tz_setup, tz_teardown },
    { "tz/fixed_offset",         test_tz_fixed_offset,         tz_setup, tz_teardown },
    { "tz/julian_rules",         test_tz_julian_rules,         tz_setup, tz_teardown },
    { "tz/out_of_range_changes", test_tz_out_of_range_changes, tz_setup, tz_teardown },
    { "tz/malformed",            test_tz_malformed,            tz_setup, tz_teardown },
    { "tz/data/every_zone_parses", test_tzdata_every_zone_parses, tz_setup, tz_teardown },
    { "tz/data/lookup",          test_tzdata_lookup,           NULL,     NULL },
    { "tz/data/known_offsets",   test_tzdata_known_offsets,    tz_setup, tz_teardown },
    { NULL, NULL, NULL, NULL },
};
