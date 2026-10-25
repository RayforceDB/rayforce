#include "lang/read_budget.h"
#include "mem/heap.h"
#include "mem/sys.h"
#include <string.h>
#include <stdint.h>
#include <assert.h>

#define READ_RECORDS 64
typedef struct { void* ptr; size_t charge; bool sys; } read_record_t;
struct ray_read_records {
    ray_read_records_t* next;
    size_t charge;
    read_record_t entries[READ_RECORDS];
};

bool ray_read_budget_init(ray_read_budget_t* b, size_t limit) {
    memset(b, 0, sizeof(*b));
    b->limit = limit;
    b->fail_after = SIZE_MAX;
    if (limit < sizeof(*b)) { b->status = RAY_READ_LIMIT; return false; }
    b->live = b->peak = sizeof(*b);
    return true;
}

static bool reserve(ray_read_budget_t* b, size_t charge) {
    if (b->status != RAY_READ_OK) return false;
    if (b->live > b->limit || charge > b->limit - b->live) {
        b->status = RAY_READ_LIMIT;
        return false;
    }
    b->live += charge;
    if (b->live > b->peak) b->peak = b->live;
    return true;
}

static bool acquire_fails(ray_read_budget_t* b) {
    /* Context-local fault injection after admission, before acquisition.
     * The ordinary allocator and all unrelated callers are untouched. */
    return b->attempts++ == b->fail_after;
}

static read_record_t* slot(ray_read_budget_t* b) {
    if (b->status != RAY_READ_OK) return NULL;
    for (ray_read_records_t* c = b->records; c; c = c->next)
        for (size_t i = 0; i < READ_RECORDS; i++)
            if (!c->entries[i].ptr) return &c->entries[i];
    size_t charge;
    if (!ray_sys_charge(sizeof(ray_read_records_t), &charge) || !reserve(b, charge)) return NULL;
    ray_read_records_t* c = acquire_fails(b) ? NULL : ray_sys_alloc(sizeof(*c));
    if (!c) { b->live -= charge; b->status = RAY_READ_OOM; return NULL; }
    c->next = b->records;
    c->charge = charge;
    b->records = c;
    return &c->entries[0];
}

void* ray_read_sys_alloc(ray_read_budget_t* b, size_t bytes) {
    read_record_t* r = slot(b);
    size_t charge;
    if (!r) return NULL;
    if (!ray_sys_charge(bytes, &charge)) { b->status = RAY_READ_LIMIT; return NULL; }
    if (!reserve(b, charge)) return NULL;
    void* p = acquire_fails(b) ? NULL : ray_sys_alloc(bytes);
    if (!p) { b->live -= charge; b->status = RAY_READ_OOM; return NULL; }
    *r = (read_record_t){p, charge, true};
    return p;
}

ray_t* ray_read_alloc(ray_read_budget_t* b, size_t bytes) {
    read_record_t* r = slot(b);
    size_t charge;
    if (!r) return NULL;
    if (!ray_alloc_charge(bytes, &charge)) { b->status = RAY_READ_LIMIT; return NULL; }
    if (!reserve(b, charge)) return NULL;
    size_t actual = 0;
    ray_t* v = acquire_fails(b) ? NULL : ray_alloc_admitted(bytes, charge, &actual);
    if (!v) { b->live -= charge; b->status = RAY_READ_OOM; return NULL; }
    assert(actual == charge);
    v->type = RAY_U8;
    v->len = 0;
    *r = (read_record_t){v, charge, false};
    return v;
}

static void remove_record(ray_read_budget_t* b, void* p, bool sys, bool transfer) {
    if (!p) return;
    for (ray_read_records_t* c = b->records; c; c = c->next) {
        for (size_t i = 0; i < READ_RECORDS; i++) {
            read_record_t* r = &c->entries[i];
            if (r->ptr != p || r->sys != sys) continue;
            if (!transfer) {
                if (sys) ray_sys_free(p); else ray_release(p);
            }
            b->live -= r->charge;
            r->ptr = NULL;
            return;
        }
    }
    assert(!"reader block not owned by this context");
}

void ray_read_sys_free(ray_read_budget_t* b, void* p) { remove_record(b, p, true, false); }
void ray_read_sys_transfer(ray_read_budget_t* b, void* p) { remove_record(b, p, true, true); }
void ray_read_release(ray_read_budget_t* b, ray_t* p) { remove_record(b, p, false, false); }

void ray_read_budget_close(ray_read_budget_t* b) {
    ray_read_records_t* c = b->records;
    while (c) {
        ray_read_records_t* next = c->next;
        for (size_t i = 0; i < READ_RECORDS; i++) {
            read_record_t* r = &c->entries[i];
            if (r->ptr) {
                if (r->sys) ray_sys_free(r->ptr); else ray_release(r->ptr);
                b->live -= r->charge;
            }
        }
        b->live -= c->charge;
        ray_sys_free(c);
        c = next;
    }
    b->records = NULL;
    /* A failed init owns no stack-context charge. */
    if (b->live) { assert(b->live == sizeof(*b)); b->live = 0; }
}

ray_t* ray_read_string(ray_read_budget_t* b, const char* s, size_t n) {
    if ((!s && n) || n > INT64_MAX || n == SIZE_MAX) { b->status = RAY_READ_INVALID; return NULL; }
    ray_t* chars = NULL;
    if (n >= 7) {
        chars = ray_read_alloc(b, n + 1);
        if (!chars) return NULL;
        chars->len = (int64_t)n;
        memcpy(ray_data(chars), s, n);
        ((char*)ray_data(chars))[n] = 0;
    }
    ray_t* v = ray_read_alloc(b, 0);
    if (!v) { ray_read_release(b, chars); return NULL; }
    v->type = -RAY_STR;
    if (chars) { ray_retain(chars); v->obj = chars; }
    else {
        v->slen = (uint8_t)n;
        if (n) memcpy(v->sdata, s, n);
        v->sdata[n] = 0;
    }
    return v;
}

ray_t* ray_read_i64(ray_read_budget_t* b, const int64_t* values, size_t n) {
    if ((!values && n) || n > INT64_MAX || n > SIZE_MAX / sizeof(int64_t)) {
        b->status = RAY_READ_INVALID; return NULL;
    }
    ray_t* v = ray_read_alloc(b, n * sizeof(int64_t));
    if (!v) return NULL;
    v->type = RAY_I64;
    v->len = (int64_t)n;
    if (n) memcpy(ray_data(v), values, n * sizeof(int64_t));
    for (size_t i = 0; i < n; i++) if (values[i] == INT64_MIN) v->attrs |= RAY_ATTR_HAS_NULLS;
    return v;
}

ray_t* ray_read_list(ray_read_budget_t* b, ray_t* const* values, size_t n) {
    if ((!values && n) || n > INT64_MAX || n > SIZE_MAX / sizeof(ray_t*)) {
        b->status = RAY_READ_INVALID; return NULL;
    }
    for (size_t i = 0; i < n; i++)
        if (!values[i] || RAY_IS_ERR(values[i])) { b->status = RAY_READ_INVALID; return NULL; }
    ray_t* v = ray_read_alloc(b, n * sizeof(ray_t*));
    if (!v) return NULL;
    v->type = RAY_LIST;
    for (size_t i = 0; i < n; i++) {
        ray_retain(values[i]); ((ray_t**)ray_data(v))[i] = values[i];
    }
    v->len = (int64_t)n;
    return v;
}

void* ray_read_sys_resize(ray_read_budget_t* b, void* old, size_t old_bytes, size_t bytes) {
    /* Old remains owned/charged until replacement succeeds. The explicit
     * proof API receives the valid source length, not allocation capacity. */
    void* p = ray_read_sys_alloc(b, bytes);
    if (!p) return NULL;
    if (old && old_bytes && bytes) memcpy(p, old, old_bytes < bytes ? old_bytes : bytes);
    ray_read_sys_free(b, old);
    return p;
}
