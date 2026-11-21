/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   Call profiler (see callprof.h).
 *
 *   State is the main thread's alone: sites, the lookup tables and the
 *   frame stack are allocated on first enable and written only by the main
 *   VM.
 *
 *   A site is the code at a source position: the file, the call's line and
 *   column, and what is there.  At top level that is the callee's name when
 *   the call's head is one (so `(f 1)` and `(f 2)` share a row); inside a
 *   lambda, the lambda's own position and the call's offset.  Every IPC
 *   request and REPL line is a fresh parse from line 1 of `repl` (and a
 *   fresh compile of any lambda it writes), so there the position says
 *   little: a hash of the text (a top-level call's without a named head, a
 *   lambda's) tells sites apart.  Keying by node or bytecode would add
 *   sites, and pin a parse tree, per request; by content, repeats aggregate.
 *
 *   Two caches put a call on its site without reading the source info:
 *   compiled calls by (bytecode, offset), each slot holding a reference so
 *   the address is not reused under it; top-level calls by node, valid for
 *   the running source alone (its nodes live as long as it does).  A site
 *   holds a reference to the last bytecode seen at it, to name it.  Slots
 *   for bytecode other than their site's are bounded: past CP_DUPS_MAX the
 *   compiled cache is dropped and refills, so requests that compile lambdas
 *   pin at most that many beside the sites'.
 */

#include "lang/callprof.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/profile.h"
#include "core/qstats.h"
#include "core/runtime.h"
#include "lang/env.h"
#include "lang/eval.h"
#include "lang/nfo.h"
#include "mem/sys.h"
#include "table/sym.h"

_Atomic(bool) g_ray_callprof = false;

/* Lookup slots: powers of two, at most about a third (compiled cache) or
 * half (positions) full, so a probe always ends at a free slot. */
#define CP_SLOTS    (RAY_CALLPROF_SITES_MAX * 4)
#define CP_PSLOTS   (RAY_CALLPROF_SITES_MAX * 2)
#define CP_NSLOTS   4096                        /* node cache, direct-mapped */
#define CP_DUPS_MAX 64
#define CP_OTHER    RAY_CALLPROF_SITES_MAX      /* the overflow row's index */

typedef struct {
    ray_t*   key;       /* retained: last bytecode seen (VM site); NULL: top */
    int32_t  ip;        /* call offset; -1 for a top-level site */
    int32_t  line;      /* 1-based source line, 0 unknown */
    int32_t  def_line;  /* VM site: the lambda's first line, for its label */
    int64_t  callee;    /* top site: the head's name symbol, or -1 */
    int32_t  active;    /* activations open: total counts the outermost */
    int64_t  file;      /* symbol: the source's file name */
    uint64_t calls;
    int64_t  total_ns;
    int64_t  self_ns;
    int64_t  busy_ns;   /* worker busy time, exclusive of nested sites */
} cp_site_t;

typedef struct {        /* compiled cache: (bytecode, offset) -> site */
    ray_t*   key;       /* retained; NULL: empty */
    int32_t  ip;
    int32_t  idx;
} cp_slot_t;

typedef struct {        /* site by position */
    int64_t  file;      /* symbol; or the bytecode's address when it has no
                           debug info (def is then CP_NODBG) */
    uint32_t pos;       /* call: start line << 16 | start column */
    uint32_t def;       /* lambda's position, 0 at top level */
    int32_t  ip;
    int32_t  idx;       /* site index + 1; 0: empty */
    uint64_t what;      /* top: the callee's symbol, else a hash of the text
                           (REPL/IPC text only; 0 elsewhere) */
} cp_pslot_t;
#define CP_NODBG UINT32_MAX

typedef struct {        /* top-level node -> site, under source `ngen` */
    const ray_t* node;
    uint32_t     gen;
    int32_t      idx;   /* site index, or -1: a node with no span */
} cp_nslot_t;

typedef struct {
    int32_t  site;
    uint32_t gen;
    int64_t  t0;
    uint64_t busy0;
    int64_t  child_ns;
    uint64_t child_busy;
} cp_frame_t;

typedef struct {
    cp_site_t  sites[RAY_CALLPROF_SITES_MAX + 1];
    cp_slot_t  slots[CP_SLOTS];
    cp_pslot_t pslots[CP_PSLOTS];
    cp_nslot_t nodes[CP_NSLOTS];
    cp_frame_t stack[RAY_CALLPROF_DEPTH_MAX];
    int32_t    n_sites;
    int32_t    n_dups;      /* compiled slots not on their site's bytecode */
    int32_t    sp;
    uint32_t   gen;         /* bumped by reset: frames of before skip */
    uint32_t   ngen;        /* node cache generation: bumped per source */
    ray_t*     nfo;         /* retained: the source the node cache is for */
    int64_t    nfo_file;    /* its file name symbol */
    int64_t    repl_file;   /* the symbol of RAY_NFO_REPL_NAME */
} cp_state_t;

static cp_state_t* g_cp;

static ray_vm_t* cp_main_vm(void) {
    return __RUNTIME && __RUNTIME->n_vms > 0 ? __RUNTIME->vms[0] : NULL;
}

bool ray_callprof_owner(void) {
    return __VM && __VM == cp_main_vm();
}

static uint32_t cp_hash(const ray_t* key, int32_t ip) {
    uint64_t h = ((uint64_t)(uintptr_t)key >> 4) ^ ((uint64_t)(uint32_t)ip * 0x9E3779B97F4A7C15ull);
    h ^= h >> 29;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 32;
    return (uint32_t)h & (CP_SLOTS - 1);
}

static cp_slot_t* cp_find(const ray_t* key, int32_t ip) {
    for (uint32_t h = cp_hash(key, ip);; h = (h + 1) & (CP_SLOTS - 1)) {
        cp_slot_t* s = &g_cp->slots[h];
        if (!s->key || (s->key == key && s->ip == ip)) return s;
    }
}

static void cp_drop_slots(void) {
    for (int32_t i = 0; i < CP_SLOTS; i++)
        if (g_cp->slots[i].key) ray_release(g_cp->slots[i].key);
    memset(g_cp->slots, 0, sizeof g_cp->slots);
    g_cp->n_dups = 0;
}

static int64_t cp_file_sym(ray_t* nfo) {
    ray_t* f = nfo ? NFO_FILENAME(nfo) : NULL;
    if (!f || RAY_IS_ERR(f) || f->type != -RAY_STR) return ray_sym_intern("", 0);
    return ray_sym_intern(ray_str_ptr(f), ray_str_len(f));
}

static int32_t cp_line(ray_span_t span) {
    return span.id ? (int32_t)span.start_line + 1 : 0;
}

static uint32_t cp_pos(ray_span_t span) {
    return (uint32_t)span.start_line << 16 | span.start_col;
}

/* Byte offset of (line, col) in src, clamped to its length. */
static size_t cp_offset(const char* src, size_t len, uint32_t line, uint32_t col) {
    size_t i = 0;
    for (uint32_t l = 0; l < line && i < len; i++) if (src[i] == '\n') l++;
    return i + col < len ? i + col : len;
}

/* Hash of the text `span` covers in nfo's source, for text that is parsed
 * from line 1 again and again (REPL lines, IPC requests): only there do
 * positions collide.  0 for other sources, which positions tell apart. */
static uint64_t cp_text_hash(ray_t* nfo, int64_t file, ray_span_t span) {
    if (!nfo || file != g_cp->repl_file) return 0;
    ray_t* src = NFO_SOURCE(nfo);
    if (!src || RAY_IS_ERR(src) || src->type != -RAY_STR) return 0;
    const char* p = ray_str_ptr(src);
    size_t len = ray_str_len(src);
    size_t a = cp_offset(p, len, span.start_line, span.start_col);
    size_t b = cp_offset(p, len, span.end_line, span.end_col);
    uint64_t h = 0xCBF29CE484222325ull;   /* FNV-1a; 0 is kept for "none" */
    for (size_t i = a; i < b; i++) h = (h ^ (uint8_t)p[i]) * 0x100000001B3ull;
    return h | 1;
}

/* The site at a position, made on first sight; the overflow row once the
 * sites are spent.  *made says which. */
static int32_t cp_site_at(int64_t file, uint32_t pos, uint32_t def, int32_t ip, uint64_t what,
                          bool* made) {
    uint64_t h = (uint64_t)file * 0x9E3779B97F4A7C15ull
               ^ ((uint64_t)pos << 32 | def) * 0xBF58476D1CE4E5B9ull
               ^ (uint64_t)(uint32_t)ip * 0x94D049BB133111EBull ^ what;
    h ^= h >> 31;
    *made = false;
    for (uint32_t i = (uint32_t)h & (CP_PSLOTS - 1);; i = (i + 1) & (CP_PSLOTS - 1)) {
        cp_pslot_t* t = &g_cp->pslots[i];
        if (t->idx) {
            if (t->file == file && t->pos == pos && t->def == def && t->ip == ip && t->what == what)
                return t->idx - 1;
            continue;
        }
        if (g_cp->n_sites >= RAY_CALLPROF_SITES_MAX) return CP_OTHER;
        int32_t idx = g_cp->n_sites++;
        cp_site_t* s = &g_cp->sites[idx];
        memset(s, 0, sizeof *s);
        s->ip = ip;
        s->file = file;
        s->callee = -1;
        *t = (cp_pslot_t){ file, pos, def, ip, idx + 1, what };
        *made = true;
        return idx;
    }
}

/* Point the node cache at `nfo`: nodes of the source before may be gone. */
static void cp_switch_nfo(ray_t* nfo) {
    if (nfo) ray_retain(nfo);
    if (g_cp->nfo) ray_release(g_cp->nfo);
    g_cp->nfo = nfo;
    g_cp->nfo_file = cp_file_sym(nfo);
    if (++g_cp->ngen == 0) {
        memset(g_cp->nodes, 0, sizeof g_cp->nodes);
        g_cp->ngen = 1;
    }
}

static int32_t cp_push(int32_t site) {
    if (g_cp->sp >= RAY_CALLPROF_DEPTH_MAX) return -1;
    int32_t d = g_cp->sp++;
    cp_frame_t* f = &g_cp->stack[d];
    f->site = site;
    f->gen = g_cp->gen;
    f->child_ns = 0;
    f->child_busy = 0;
    g_cp->sites[site].active++;
    f->busy0 = ray_qstats_calls_busy();
    f->t0 = ray_profile_now_ns();   /* last: the frame's own setup is not timed */
    return d;
}

/* Cache miss: the site of (fn's bytecode, ip) by position.  *dup: the
 * site held other code, whose slots it now does not keep alive. */
static int32_t cp_vm_site(ray_t* fn, ray_t* bc, int32_t ip, bool* dup) {
    ray_t* dbg = LAMBDA_DBG(fn);
    ray_span_t call = ray_bc_dbg_get(dbg, ip);
    ray_span_t def = ray_bc_dbg_get(dbg, 0);
    bool made;
    int32_t site;
    if (call.id && def.id) {
        ray_t* nfo = LAMBDA_NFO(fn);
        int64_t file = cp_file_sym(nfo);
        site = cp_site_at(file, cp_pos(call), cp_pos(def), ip, cp_text_hash(nfo, file, def), &made);
    } else {
        site = cp_site_at((int64_t)(uintptr_t)bc, 0, CP_NODBG, ip, 0, &made);
    }
    *dup = site == CP_OTHER;
    if (site == CP_OTHER) return site;
    cp_site_t* s = &g_cp->sites[site];
    if (made) {
        if (!call.id || !def.id) s->file = cp_file_sym(LAMBDA_NFO(fn));
        s->line = cp_line(call);
        s->def_line = cp_line(def);
    }
    if (s->key != bc) {       /* name the site by the newest code at it */
        *dup = s->key != NULL;
        ray_retain(bc);
        if (s->key) ray_release(s->key);
        s->key = bc;
    }
    return site;
}

int32_t ray_callprof_enter_vm(ray_t* fn, int32_t ip) {
    /* The owner test first: g_cp is the main thread's to read. */
    if (!ray_callprof_owner() || !g_cp || !fn || fn->type != RAY_LAMBDA) return -1;
    ray_t* bc = LAMBDA_BC(fn);
    if (!bc) return -1;
    cp_slot_t* slot = cp_find(bc, ip);
    if (slot->key) return cp_push(slot->idx);
    bool dup;
    int32_t site = cp_vm_site(fn, bc, ip, &dup);
    /* Slots for code no site holds (the code before at a repeated position,
     * calls in the overflow row) are bounded. */
    if (dup && g_cp->n_dups >= CP_DUPS_MAX) {
        cp_drop_slots();
        slot = cp_find(bc, ip);
    }
    ray_retain(bc);
    *slot = (cp_slot_t){ bc, ip, site };
    g_cp->n_dups += dup;
    return cp_push(site);
}

int32_t ray_callprof_enter_node(ray_t* node) {
    if (!ray_callprof_owner() || !g_cp || __VM->lambda_depth != 0) return -1;
    ray_t* nfo = __VM->nfo;
    if (nfo != g_cp->nfo) cp_switch_nfo(nfo);
    cp_nslot_t* c = &g_cp->nodes[cp_hash(node, -1) & (CP_NSLOTS - 1)];
    if (c->node != node || c->gen != g_cp->ngen) {
        /* Nodes without a span (built by C, not parsed) are not timed. */
        ray_span_t span = ray_nfo_get(nfo, node);
        int32_t site = -1;
        if (span.id) {
            /* The callee's name, when the head is one, keeps `(f 1)` and
             * `(f 2)` on one row and `(f 1)`, `(g 1)` on two. */
            ray_t* head = node->type == RAY_LIST && node->len > 0 ? ((ray_t**)ray_data(node))[0] : NULL;
            int64_t callee = head && head->type == -RAY_SYM && !(head->attrs & ATTR_QUOTED) ? head->i64 : -1;
            uint64_t what = callee >= 0 ? (uint64_t)callee + 1 : cp_text_hash(nfo, g_cp->nfo_file, span);
            bool made;
            site = cp_site_at(g_cp->nfo_file, cp_pos(span), 0, -1, what, &made);
            if (made && site != CP_OTHER) {
                g_cp->sites[site].line = cp_line(span);
                g_cp->sites[site].callee = callee;
            }
        }
        *c = (cp_nslot_t){ node, g_cp->ngen, site };
    }
    return c->idx < 0 ? -1 : cp_push(c->idx);
}

void ray_callprof_exit(int32_t frame) {
    if (frame < 0 || !g_cp || frame >= g_cp->sp) return;
    int64_t now = ray_profile_now_ns();
    uint64_t busy = ray_qstats_calls_busy();
    cp_frame_t* f = &g_cp->stack[frame];
    g_cp->sp = frame;
    if (f->gen != g_cp->gen) return;      /* reset while the call ran */
    int64_t total = now - f->t0;
    uint64_t busy_total = busy - f->busy0;
    cp_site_t* s = &g_cp->sites[f->site];
    s->calls++;
    s->self_ns += total - f->child_ns;
    s->busy_ns += (int64_t)(busy_total - f->child_busy);
    if (--s->active == 0) s->total_ns += total;
    if (frame > 0) {
        cp_frame_t* p = &g_cp->stack[frame - 1];
        p->child_ns += total;
        p->child_busy += busy_total;
    }
}

static void cp_release_sites(void) {
    for (int32_t i = 0; i < g_cp->n_sites; i++)
        if (g_cp->sites[i].key) ray_release(g_cp->sites[i].key);
    g_cp->n_sites = 0;
    cp_drop_slots();
    memset(g_cp->pslots, 0, sizeof g_cp->pslots);
    cp_switch_nfo(NULL);    /* the cached nodes name dropped sites */
    memset(&g_cp->sites[CP_OTHER], 0, sizeof g_cp->sites[CP_OTHER]);
    g_cp->gen++;
}

static int32_t cp_row_count(void) {
    int32_t n = 0;
    for (int32_t i = 0; g_cp && i < g_cp->n_sites; i++) n += g_cp->sites[i].calls > 0;
    return n;
}

int32_t ray_callprof_depth(void) {
    return g_cp ? g_cp->sp : 0;
}

void ray_callprof_set_enabled(bool on) {
    if (on && !g_cp) {
        g_cp = (cp_state_t*)ray_sys_alloc(sizeof(cp_state_t));
        if (!g_cp) return;
        g_cp->repl_file = ray_sym_intern(RAY_NFO_REPL_NAME, strlen(RAY_NFO_REPL_NAME));
    }
    ray_qstats_set_calls(on);
    atomic_store_explicit(&g_ray_callprof, on, memory_order_relaxed);
}

void ray_callprof_reset(void) {
    if (g_cp) cp_release_sites();
}

void ray_callprof_destroy(void) {
    atomic_store_explicit(&g_ray_callprof, false, memory_order_relaxed);
    ray_qstats_set_calls(false);
    if (!g_cp) return;
    cp_release_sites();
    ray_sys_free(g_cp);
    g_cp = NULL;
}

/* ── names: the global a lambda's bytecode is bound under ── */

typedef struct { const ray_t* bc; int64_t name; } cp_name_t;

typedef struct {
    cp_name_t* v;
    int32_t    n, cap;
} cp_names_t;

static void cp_names_add(cp_names_t* ns, const ray_t* bc, int64_t name) {
    if (ns->n == ns->cap) {
        int32_t cap = ns->cap ? ns->cap * 2 : 256;
        cp_name_t* v = (cp_name_t*)ray_sys_alloc((size_t)cap * sizeof *v);
        if (!v) return;
        if (ns->v) { memcpy(v, ns->v, (size_t)ns->n * sizeof *v); ray_sys_free(ns->v); }
        ns->v = v;
        ns->cap = cap;
    }
    ns->v[ns->n++] = (cp_name_t){ bc, name };
}

/* Lambdas bound at `prefix` (a symbol, -1 for the globals) and, through
 * dicts (dotted names), up to four levels below it. */
static void cp_names_walk(cp_names_t* ns, int64_t prefix, int64_t key, ray_t* val, int depth) {
    if (!val || RAY_IS_ERR(val)) return;
    int64_t name = key;
    if (prefix >= 0) {
        ray_t* a = ray_sym_str(prefix);
        ray_t* b = ray_sym_str(key);
        if (!a || !b) return;
        char buf[256];
        int len = snprintf(buf, sizeof buf, "%.*s.%.*s",
                           (int)ray_str_len(a), ray_str_ptr(a), (int)ray_str_len(b), ray_str_ptr(b));
        if (len <= 0 || len >= (int)sizeof buf) return;
        name = ray_sym_intern(buf, (size_t)len);
    }
    if (val->type == RAY_LAMBDA) {
        if (LAMBDA_BC(val)) cp_names_add(ns, LAMBDA_BC(val), name);
        return;
    }
    if (val->type != RAY_DICT || depth >= 4) return;
    ray_t* keys = ray_dict_keys(val);
    ray_t* vals = ray_dict_vals(val);
    if (!keys || !vals || keys->type != RAY_SYM || vals->type != RAY_LIST || keys->len != vals->len) return;
    for (int64_t i = 0; i < keys->len; i++)
        cp_names_walk(ns, name, ray_read_sym(ray_data(keys), i, RAY_SYM, keys->attrs),
                      ((ray_t**)ray_data(vals))[i], depth + 1);
}

static int cp_name_cmp(const void* a, const void* b) {
    uintptr_t x = (uintptr_t)((const cp_name_t*)a)->bc, y = (uintptr_t)((const cp_name_t*)b)->bc;
    return x < y ? -1 : x > y;
}

static int64_t cp_names_find(const cp_names_t* ns, const ray_t* bc) {
    int32_t lo = 0, hi = ns->n;
    while (lo < hi) {
        int32_t mid = lo + (hi - lo) / 2;
        if ((uintptr_t)ns->v[mid].bc < (uintptr_t)bc) lo = mid + 1;
        else hi = mid;
    }
    return lo < ns->n && ns->v[lo].bc == bc ? ns->v[lo].name : -1;
}

/* Rows by self time, highest first; equal times keep first-call order. */
static const cp_state_t* g_cp_sort;
static int cp_row_cmp(const void* a, const void* b) {
    int32_t i = *(const int32_t*)a, j = *(const int32_t*)b;
    int64_t x = g_cp_sort->sites[i].self_ns, y = g_cp_sort->sites[j].self_ns;
    if (x != y) return x > y ? -1 : 1;
    return i < j ? -1 : i > j;
}

ray_t* ray_callprof_table(void) {
    /* Sites whose every call is still open (the one reading the table,
     * say) have not run yet: no row. */
    int32_t n = cp_row_count();
    bool other = g_cp && g_cp->sites[CP_OTHER].calls > 0;
    int64_t nrows = n + (other ? 1 : 0);

    cp_names_t names = {0};
    if (n > 0) {
        int32_t ng = ray_env_global_count();
        int64_t* syms = ng > 0 ? (int64_t*)ray_sys_alloc((size_t)ng * sizeof(int64_t)) : NULL;
        ray_t** vals = ng > 0 ? (ray_t**)ray_sys_alloc((size_t)ng * sizeof(ray_t*)) : NULL;
        if (syms && vals) {
            ng = ray_env_list(syms, vals, ng);
            for (int32_t i = 0; i < ng; i++) cp_names_walk(&names, -1, syms[i], vals[i], 0);
        }
        if (syms) ray_sys_free(syms);
        if (vals) ray_sys_free(vals);
        if (names.n > 1) qsort(names.v, (size_t)names.n, sizeof *names.v, cp_name_cmp);
    }

    ray_t* c_file  = ray_vec_new(RAY_SYM, nrows);
    ray_t* c_line  = ray_vec_new(RAY_I64, nrows);
    ray_t* c_fn    = ray_vec_new(RAY_SYM, nrows);
    ray_t* c_calls = ray_vec_new(RAY_I64, nrows);
    ray_t* c_self  = ray_vec_new(RAY_F64, nrows);
    ray_t* c_total = ray_vec_new(RAY_F64, nrows);
    ray_t* c_busy  = ray_vec_new(RAY_F64, nrows);
    int32_t* order = nrows > 0 ? (int32_t*)ray_sys_alloc((size_t)nrows * sizeof(int32_t)) : NULL;
    ray_t* cols[] = { c_file, c_line, c_fn, c_calls, c_self, c_total, c_busy };
    enum { NCOLS = sizeof cols / sizeof cols[0] };
    bool bad = nrows > 0 && !order;
    for (int i = 0; i < NCOLS; i++) if (!cols[i] || RAY_IS_ERR(cols[i])) bad = true;
    if (bad) {
        for (int i = 0; i < NCOLS; i++) if (cols[i] && !RAY_IS_ERR(cols[i])) ray_release(cols[i]);
        if (order) ray_sys_free(order);
        if (names.v) ray_sys_free(names.v);
        return ray_error("oom", ".sys.profile: column alloc");
    }

    for (int32_t i = 0, r = 0; r < n; i++) if (g_cp->sites[i].calls > 0) order[r++] = i;
    if (n > 1) { g_cp_sort = g_cp; qsort(order, (size_t)n, sizeof *order, cp_row_cmp); }
    if (other) order[n] = CP_OTHER;

    int64_t top = ray_sym_intern("<top>", 5);
    for (int64_t r = 0; r < nrows; r++) {
        const cp_site_t* s = &g_cp->sites[order[r]];
        int64_t fn;
        if (order[r] == CP_OTHER) fn = ray_sym_intern("<other>", 7);
        else if (s->ip < 0 && s->callee >= 0) {
            ray_t* name = ray_sym_str(s->callee);
            char buf[256];
            int len = name ? snprintf(buf, sizeof buf, "<top>%.*s", (int)ray_str_len(name), ray_str_ptr(name)) : -1;
            fn = len > 0 && len < (int)sizeof buf ? ray_sym_intern(buf, (size_t)len) : top;
        }
        else if (s->ip < 0) fn = top;
        else {
            fn = cp_names_find(&names, s->key);
            if (fn < 0) {
                char buf[32];
                int len = snprintf(buf, sizeof buf, "<lambda@%d>", (int)s->def_line);
                fn = ray_sym_intern(buf, (size_t)len);
            }
        }
        ((int64_t*)ray_data(c_file))[r]  = order[r] == CP_OTHER ? ray_sym_intern("", 0) : s->file;
        ((int64_t*)ray_data(c_line))[r]  = s->line;
        ((int64_t*)ray_data(c_fn))[r]    = fn;
        ((int64_t*)ray_data(c_calls))[r] = (int64_t)s->calls;
        ((double*)ray_data(c_self))[r]   = (double)s->self_ns / 1e6;
        ((double*)ray_data(c_total))[r]  = (double)s->total_ns / 1e6;
        ((double*)ray_data(c_busy))[r]   = (double)s->busy_ns / 1e6;
    }
    if (order) ray_sys_free(order);
    if (names.v) ray_sys_free(names.v);
    for (int i = 0; i < NCOLS; i++) cols[i]->len = nrows;

    static const char* col_names[] = { "file", "line", "fn", "calls", "self-ms", "total-ms", "busy-ms" };
    ray_t* tbl = ray_table_new(NCOLS);
    if (!tbl || RAY_IS_ERR(tbl)) {
        for (int i = 0; i < NCOLS; i++) ray_release(cols[i]);
        return tbl ? tbl : ray_error("oom", ".sys.profile: table");
    }
    for (int i = 0; i < NCOLS; i++) {
        tbl = ray_table_add_col(tbl, ray_sym_intern(col_names[i], strlen(col_names[i])), cols[i]);
        ray_release(cols[i]);
        if (RAY_IS_ERR(tbl)) {
            for (int j = i + 1; j < NCOLS; j++) ray_release(cols[j]);
            return tbl;
        }
    }
    return tbl;
}
