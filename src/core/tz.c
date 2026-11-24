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

#include "core/tz.h"
#include "lang/cal.h"   /* the civil calendar: ymd_to_date, date_to_ymd, MONTHDAYS */
#include "mem/sys.h"

#include <string.h>

#define TZ_HDR          44            /* "TZif", version, 15 reserved, 6 counts */
#define TZ_EPOCH_2000   946684800LL   /* 2000.01.01 in Unix seconds */
#define TZ_SEC_MAX      9223372035LL  /* seconds from 2000 a TIMESTAMP holds */
#define TZ_YEAR_FIRST   1707          /* the TIMESTAMP range, in whole years */
#define TZ_YEAR_LAST    2293
#define TZ_TIMECNT_MAX  100000u
#define TZ_OFF_MAX      172800        /* no real zone is two days from UTC */

static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static int64_t be64(const uint8_t* p) {
    return (int64_t)(((uint64_t)be32(p) << 32) | be32(p + 4));
}

bool ray_tz_name_ok(const char* p, size_t n) {
    if (!p || n == 0 || n > 64) return false;
    bool seg_start = true;
    for (size_t i = 0; i < n; i++) {
        char c = p[i];
        if (c == '/') {
            if (seg_start) return false;
            seg_start = true;
            continue;
        }
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '+' || c == '-';
        if (!ok) return false;
        seg_start = false;
    }
    return !seg_start;
}

/* Days from 1970.01.01 to a civil date. */
static int64_t tz_days_from_civil(int64_t y, int m, int d) {
    return (int64_t)ymd_to_date((int)y, m, d) + RAY_DATE_EPOCH_UNIX_DAYS;
}

/* ---- The POSIX TZ string a file ends with -------------------------------- */

typedef struct {
    char    kind;    /* 'M' month.week.day, 'J' day 1..365 with no Feb 29, 'N' day 0..365 */
    int     m, w, d; /* 'M': month 1..12, week 1..5 (5 is the last), weekday 0 = Sunday */
    int     n;       /* 'J', 'N' */
    int32_t time;    /* seconds after local midnight, -167h..167h */
} tz_rule_t;

typedef struct {
    int32_t   std_off, dst_off;   /* seconds east of UTC */
    bool      has_dst;
    tz_rule_t start, end;         /* when DST starts, on standard time; ends, on DST */
} tz_posix_t;

static bool tzs_name(const char** p, const char* e) {
    const char* s = *p;
    const char* b;
    if (s < e && *s == '<') {
        b = ++s;
        while (s < e && *s != '>') s++;
        if (s == e || s - b < 3) return false;
        *p = s + 1;
        return true;
    }
    b = s;
    while (s < e && ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z'))) s++;
    if (s - b < 3) return false;
    *p = s;
    return true;
}

static bool tzs_int(const char** p, const char* e, int lo, int hi, int* out) {
    const char* s = *p;
    const char* b = s;
    int x = 0;
    while (s < e && *s >= '0' && *s <= '9' && s - b < 3) x = x * 10 + (*s++ - '0');
    if (s == b || x < lo || x > hi) return false;
    *out = x;
    *p = s;
    return true;
}

/* [+-]h[hh][:mm[:ss]] in seconds. */
static bool tzs_hms(const char** p, const char* e, int hour_max, int32_t* out) {
    const char* s = *p;
    int sign = 1, v[3] = {0, 0, 0};
    if (s < e && (*s == '+' || *s == '-')) sign = *s++ == '-' ? -1 : 1;
    if (!tzs_int(&s, e, 0, hour_max, &v[0])) return false;
    for (int k = 1; k < 3 && s < e && *s == ':'; k++) {
        s++;
        if (!tzs_int(&s, e, 0, 59, &v[k])) return false;
    }
    *out = sign * (v[0] * 3600 + v[1] * 60 + v[2]);
    *p = s;
    return true;
}

static bool tzs_rule(const char** p, const char* e, tz_rule_t* r) {
    const char* s = *p;
    r->time = 7200;
    if (s < e && *s == 'M') {
        s++;
        r->kind = 'M';
        if (!tzs_int(&s, e, 1, 12, &r->m) || s == e || *s++ != '.') return false;
        if (!tzs_int(&s, e, 1, 5, &r->w) || s == e || *s++ != '.') return false;
        if (!tzs_int(&s, e, 0, 6, &r->d)) return false;
    } else if (s < e && *s == 'J') {
        s++;
        r->kind = 'J';
        if (!tzs_int(&s, e, 1, 365, &r->n)) return false;
    } else {
        r->kind = 'N';
        if (!tzs_int(&s, e, 0, 365, &r->n)) return false;
    }
    if (s < e && *s == '/') {
        s++;
        if (!tzs_hms(&s, e, 167, &r->time)) return false;
    }
    *p = s;
    return true;
}

/* std offset [dst [offset] ,start[/time],end[/time]].  An offset is west
 * positive in the string and east positive in the answer.  DST with no rule
 * is refused: POSIX leaves its dates to the implementation, and no TZif
 * footer is written that way. */
static bool tz_posix_parse(const char* s, const char* e, tz_posix_t* out) {
    int32_t west;
    memset(out, 0, sizeof(*out));
    if (!tzs_name(&s, e) || !tzs_hms(&s, e, 24, &west)) return false;
    out->std_off = -west;
    if (s == e) return true;
    if (!tzs_name(&s, e)) return false;
    out->has_dst = true;
    out->dst_off = out->std_off + 3600;
    if (s < e && *s != ',') {
        if (!tzs_hms(&s, e, 24, &west)) return false;
        out->dst_off = -west;
    }
    if (s == e || *s++ != ',' || !tzs_rule(&s, e, &out->start)) return false;
    if (s == e || *s++ != ',' || !tzs_rule(&s, e, &out->end)) return false;
    return s == e;
}

/* The rule's instant in year y, as seconds of local time since 1970. */
static int64_t tz_rule_local(const tz_rule_t* r, int64_t y) {
    int64_t days;
    if (r->kind == 'M') {
        const uint32_t* cum = MONTHDAYS[date_leap_year((int)y)];
        int64_t first = tz_days_from_civil(y, r->m, 1);
        int     dow = (int)(((first % 7) + 11) % 7);   /* 1970.01.01 was a Thursday */
        int     len = (int)(cum[r->m] - cum[r->m - 1]);
        int     day = 1 + (r->d - dow + 7) % 7 + (r->w - 1) * 7;
        if (day > len) day -= 7;
        days = first + day - 1;
    } else if (r->kind == 'J') {
        days = tz_days_from_civil(y, 1, 1) + r->n - 1 + (date_leap_year((int)y) && r->n >= 60);
    } else {
        days = tz_days_from_civil(y, 1, 1) + r->n;
    }
    return days * 86400 + r->time;
}

/* ---- The file ------------------------------------------------------------- */

typedef struct {
    uint32_t isut, isstd, leap, time, type, chars;
} tz_counts_t;

static bool tz_header(const uint8_t* p, size_t len, tz_counts_t* c) {
    if (len < TZ_HDR || memcmp(p, "TZif", 4) != 0) return false;
    c->isut = be32(p + 20);
    c->isstd = be32(p + 24);
    c->leap = be32(p + 28);
    c->time = be32(p + 32);
    c->type = be32(p + 36);
    c->chars = be32(p + 40);
    return c->time <= TZ_TIMECNT_MAX && c->type >= 1 && c->type <= 256 && c->chars <= 65536 &&
           c->leap <= 65536 && c->isut <= 256 && c->isstd <= 256;
}

/* The bytes of a data block whose transition times are `w` wide. */
static size_t tz_block(const tz_counts_t* c, size_t w) {
    return (size_t)c->time * (w + 1) + (size_t)c->type * 6 + c->chars + (size_t)c->leap * (w + 4) +
           c->isstd + c->isut;
}

/* Changes in Unix seconds while the table is built. */
typedef struct {
    int64_t* at;
    int32_t* off;   /* off[i] is the offset from at[i] on */
    int32_t  n;
} tz_build_t;

/* Unroll the rule from the year of the file's last change (the range's first
 * year when the file lists none), keeping what comes after that change.
 *
 * A rule's change may fall on the very instant of the change before it: the
 * footer for DST all year round, "EST5EDT,0/0,J365/25", ends each year's DST
 * at the instant the next year's starts.  The later of the two is what holds
 * from then on, so it replaces the offset of the entry already there. */
static void tz_unroll(tz_build_t* b, const tz_posix_t* px) {
    int64_t last = b->n ? b->at[b->n - 1] : INT64_MIN;
    int64_t y0 = TZ_YEAR_FIRST;
    if (last > TZ_SEC_MAX + TZ_EPOCH_2000) return;   /* the file's changes already pass the range */
    if (last > -TZ_SEC_MAX) {   /* an earlier change is before the range; in it, the day fits cal.h's 32 bits */
        int64_t days = last >= 0 ? last / 86400 : -((-last + 86399) / 86400);
        int y, m, d;
        date_to_ymd((int32_t)(days - RAY_DATE_EPOCH_UNIX_DAYS), &y, &m, &d);
        if (y > y0) y0 = y;
    }
    for (int64_t y = y0; y <= TZ_YEAR_LAST; y++) {
        int64_t s = tz_rule_local(&px->start, y) - px->std_off;
        int64_t e = tz_rule_local(&px->end, y) - px->dst_off;
        int64_t at[2] = {s, e};
        int32_t off[2] = {px->dst_off, px->std_off};
        if (e < s) {   /* southern hemisphere: the year opens on DST */
            at[0] = e; off[0] = px->std_off;
            at[1] = s; off[1] = px->dst_off;
        }
        for (int k = 0; k < 2; k++) {
            if (at[k] < last) continue;
            if (at[k] == last && b->n) { b->off[b->n - 1] = off[k]; continue; }
            b->at[b->n] = at[k];
            b->off[b->n] = off[k];
            b->n++;
            last = at[k];
        }
    }
}

/* The built changes as the table: TIMESTAMP instants, with every change
 * before the range folded into off[0] and every one past it dropped. */
static ray_tz_t* tz_finish(const tz_build_t* b, int32_t off0) {
    size_t n = (size_t)b->n;
    ray_tz_t* z = ray_sys_alloc(sizeof(ray_tz_t) + (2 * n + 1) * sizeof(int64_t));
    if (!z) return NULL;
    z->at = (int64_t*)(z + 1);
    z->off = z->at + n;
    z->n = 0;
    z->off[0] = (int64_t)off0 * 1000000000LL;
    for (int32_t i = 0; i < b->n; i++) {
        int64_t off = (int64_t)b->off[i] * 1000000000LL;
        if (b->at[i] < -TZ_SEC_MAX + TZ_EPOCH_2000) { z->off[0] = off; continue; }
        int64_t s = b->at[i] - TZ_EPOCH_2000;
        if (s > TZ_SEC_MAX) break;
        z->at[z->n] = s * 1000000000LL;
        z->off[z->n + 1] = off;
        z->n++;
    }
    return z;
}

ray_tz_t* ray_tz_parse(const uint8_t* buf, size_t len) { return ray_tz_parse_why(buf, len, NULL); }

ray_tz_t* ray_tz_parse_why(const uint8_t* buf, size_t len, bool* oom) {
    tz_counts_t c;
    if (oom) *oom = false;
    if (!buf || !tz_header(buf, len, &c) || buf[4] < '2') return NULL;
    size_t skip = TZ_HDR + tz_block(&c, 4);   /* the 32-bit block, unread */
    if (skip > len || !tz_header(buf + skip, len - skip, &c) || c.leap != 0) return NULL;
    const uint8_t* p = buf + skip + TZ_HDR;
    size_t body = tz_block(&c, 8);
    if (body > len - skip - TZ_HDR) return NULL;
    const uint8_t* times = p;
    const uint8_t* idx = times + (size_t)c.time * 8;
    const uint8_t* types = idx + c.time;

    /* The footer: '\n', the TZ string, '\n'.  Absent or empty means the last
     * change holds for ever. */
    const char* fs = (const char*)p + body;
    const char* fe = (const char*)buf + len;
    tz_posix_t px = {0};
    if (fs < fe) {
        if (*fs++ != '\n') return NULL;
        const char* nl = memchr(fs, '\n', (size_t)(fe - fs));
        if (!nl) return NULL;
        if (nl > fs && !tz_posix_parse(fs, nl, &px)) return NULL;
    }

    size_t cap = (size_t)c.time + 2 * (TZ_YEAR_LAST - TZ_YEAR_FIRST + 1);
    tz_build_t b = {0};
    b.at = ray_sys_alloc(cap * (sizeof(int64_t) + sizeof(int32_t)));
    if (!b.at) { if (oom) *oom = true; return NULL; }
    b.off = (int32_t*)(b.at + cap);

    ray_tz_t* z = NULL;
    int32_t off0 = (int32_t)be32(types);   /* type 0 holds before the first change (RFC 8536 3.2) */
    if (off0 < -TZ_OFF_MAX || off0 > TZ_OFF_MAX) goto done;
    for (uint32_t i = 0; i < c.time; i++) {
        int64_t at = be64(times + (size_t)i * 8);
        if (idx[i] >= c.type || (i && at <= b.at[i - 1])) goto done;
        int32_t off = (int32_t)be32(types + (size_t)idx[i] * 6);
        if (off < -TZ_OFF_MAX || off > TZ_OFF_MAX) goto done;
        b.at[i] = at;
        b.off[i] = off;
    }
    b.n = (int32_t)c.time;
    if (px.has_dst) tz_unroll(&b, &px);
    z = tz_finish(&b, off0);
    if (!z && oom) *oom = true;
done:
    ray_sys_free(b.at);
    return z;
}

void ray_tz_free(ray_tz_t* z) { ray_sys_free(z); }

int32_t ray_tzdata_index(const char* name, size_t len) {
    char key[65];
    if (!ray_tz_name_ok(name, len)) return -1;   /* which also bounds len to the key */
    memcpy(key, name, len);
    key[len] = '\0';
    int32_t lo = 0, hi = ray_tzdata_count;
    while (lo < hi) {
        int32_t mid = lo + (hi - lo) / 2;
        int c = strcmp(key, ray_tzdata_zones[mid].name);
        if (c == 0) return mid;
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return -1;
}
