/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 */

#include "core/mcast.h"
#include "core/ipc.h"
#include "lang/eval.h"
#include "mem/heap.h"
#include "mem/sys.h"
#include "table/sym.h"
#include <string.h>

typedef struct {
    int64_t handle;
    ray_t*  filter;
    int64_t last_sent_seq;
    int64_t dropped;
} ray_mcast_sub_t;

typedef struct {
    int64_t topic_sym;
    int64_t next_seq;
    ray_mcast_sub_t* subs;
    int32_t n_subs;
    int32_t cap_subs;
} ray_mcast_topic_t;

struct ray_mcast {
    ray_mcast_topic_t* topics;
    int32_t n_topics;
    int32_t cap_topics;
    int64_t published;
    int64_t delivered;
    int64_t dropped;
};

static int64_t topic_sym(ray_t* topic) {
    if (!topic) return -1;
    if (ray_is_atom(topic) && topic->type == -RAY_SYM) return topic->i64;
    if (ray_is_atom(topic) && topic->type == -RAY_STR)
        return ray_sym_intern(ray_str_ptr(topic), ray_str_len(topic));
    if (topic->type == RAY_STR && ray_len(topic) == 1) {
        size_t len = 0;
        const char* s = ray_str_vec_get(topic, 0, &len);
        return s ? ray_sym_intern(s, len) : -1;
    }
    if (topic->type == RAY_SYM && ray_len(topic) == 1)
        return ray_read_sym(ray_data(topic), 0, RAY_SYM, topic->attrs);
    return -1;
}

ray_mcast_t* ray_mcast_create(void) {
    ray_mcast_t* mc = (ray_mcast_t*)ray_sys_alloc(sizeof(ray_mcast_t));
    if (!mc) return NULL;
    memset(mc, 0, sizeof(*mc));
    return mc;
}

void ray_mcast_destroy(ray_mcast_t* mc) {
    if (!mc) return;
    for (int32_t i = 0; i < mc->n_topics; i++) {
        ray_mcast_topic_t* t = &mc->topics[i];
        for (int32_t j = 0; j < t->n_subs; j++) {
            if (t->subs[j].filter && t->subs[j].filter != RAY_NULL_OBJ)
                ray_release(t->subs[j].filter);
        }
        if (t->subs) ray_sys_free(t->subs);
    }
    if (mc->topics) ray_sys_free(mc->topics);
    ray_sys_free(mc);
}

static ray_mcast_t* poll_mcast(ray_poll_t* poll) {
    if (!poll) return NULL;
    if (!poll->mcast)
        poll->mcast = ray_mcast_create();
    return (ray_mcast_t*)poll->mcast;
}

static ray_mcast_topic_t* find_topic(ray_mcast_t* mc, int64_t sym) {
    if (!mc) return NULL;
    for (int32_t i = 0; i < mc->n_topics; i++)
        if (mc->topics[i].topic_sym == sym) return &mc->topics[i];
    return NULL;
}

static ray_mcast_topic_t* ensure_topic(ray_mcast_t* mc, int64_t sym) {
    ray_mcast_topic_t* t = find_topic(mc, sym);
    if (t) return t;
    if (mc->n_topics >= mc->cap_topics) {
        int32_t new_cap = mc->cap_topics ? mc->cap_topics * 2 : 8;
        ray_mcast_topic_t* nt = (ray_mcast_topic_t*)ray_sys_alloc(
            (size_t)new_cap * sizeof(ray_mcast_topic_t));
        if (!nt) return NULL;
        if (mc->topics)
            memcpy(nt, mc->topics, (size_t)mc->n_topics * sizeof(ray_mcast_topic_t));
        memset(nt + mc->n_topics, 0,
               (size_t)(new_cap - mc->n_topics) * sizeof(ray_mcast_topic_t));
        ray_sys_free(mc->topics);
        mc->topics = nt;
        mc->cap_topics = new_cap;
    }
    t = &mc->topics[mc->n_topics++];
    memset(t, 0, sizeof(*t));
    t->topic_sym = sym;
    t->next_seq = 1;
    return t;
}

static void remove_sub(ray_mcast_topic_t* t, int32_t idx) {
    if (!t || idx < 0 || idx >= t->n_subs) return;
    if (t->subs[idx].filter && t->subs[idx].filter != RAY_NULL_OBJ)
        ray_release(t->subs[idx].filter);
    if (idx + 1 < t->n_subs)
        memmove(&t->subs[idx], &t->subs[idx + 1],
                (size_t)(t->n_subs - idx - 1) * sizeof(ray_mcast_sub_t));
    t->n_subs--;
}

static ray_t* make_upd_msg(int64_t topic, int64_t seq, ray_t* payload) {
    ray_t* msg = ray_list_new(4);
    if (!msg || RAY_IS_ERR(msg)) return msg ? msg : ray_error("oom", NULL);

    ray_t* topic_name = ray_sym_str(topic);
    if (!topic_name || RAY_IS_ERR(topic_name)) {
        if (topic_name && RAY_IS_ERR(topic_name)) ray_error_free(topic_name);
        ray_release(msg);
        return ray_error("domain", ".mc.pub topic is not interned");
    }

    ray_t* head = ray_sym(ray_sym_intern("upd", 3));
    ray_t* top  = ray_str(ray_str_ptr(topic_name), ray_str_len(topic_name));
    ray_t* s    = ray_i64(seq);
    ray_t* body = payload;
    if (payload && ray_is_atom(payload) && payload->type == -RAY_SYM) {
        body = ray_alloc_copy(payload);
        if (body && !RAY_IS_ERR(body)) body->attrs |= ATTR_QUOTED;
    }
    if (!head || RAY_IS_ERR(head) || !top || RAY_IS_ERR(top) || !s || RAY_IS_ERR(s)) {
        if (head && !RAY_IS_ERR(head)) ray_release(head);
        if (top && !RAY_IS_ERR(top)) ray_release(top);
        if (s && !RAY_IS_ERR(s)) ray_release(s);
        if (body && body != payload && !RAY_IS_ERR(body)) ray_release(body);
        ray_release(msg);
        return ray_error("oom", NULL);
    }
    if (!body || RAY_IS_ERR(body)) {
        ray_release(head);
        ray_release(top);
        ray_release(s);
        ray_release(msg);
        return body ? body : ray_error("oom", NULL);
    }

    msg = ray_list_append(msg, head); ray_release(head);
    if (RAY_IS_ERR(msg)) { if (body != payload) ray_release(body); return msg; }
    msg = ray_list_append(msg, top);  ray_release(top);
    if (RAY_IS_ERR(msg)) { if (body != payload) ray_release(body); return msg; }
    msg = ray_list_append(msg, s);    ray_release(s);
    if (RAY_IS_ERR(msg)) { if (body != payload) ray_release(body); return msg; }
    msg = ray_list_append(msg, body);
    if (body != payload) ray_release(body);
    return msg;
}

ray_t* ray_mcast_sub(ray_poll_t* poll, int64_t handle, ray_t* topic, ray_t* filter) {
    if (handle < 0) return ray_error("domain", ".mc.sub requires an active IPC connection");
    bool no_filter = (filter == RAY_NULL_OBJ) ||
                     (filter && ray_is_atom(filter) && RAY_ATOM_IS_NULL(filter));
    if (!no_filter)
        return ray_error("nyi", ".mc.sub filters are not implemented yet; pass null");
    filter = RAY_NULL_OBJ;

    int64_t sym = topic_sym(topic);
    if (sym < 0) return ray_error("type", ".mc.sub topic must be a symbol or string");

    ray_mcast_t* mc = poll_mcast(poll);
    if (!mc) return ray_error("oom", NULL);
    ray_mcast_topic_t* t = ensure_topic(mc, sym);
    if (!t) return ray_error("oom", NULL);

    for (int32_t i = 0; i < t->n_subs; i++) {
        if (t->subs[i].handle == handle) {
            if (t->subs[i].filter && t->subs[i].filter != RAY_NULL_OBJ)
                ray_release(t->subs[i].filter);
            t->subs[i].filter = filter;
            if (filter != RAY_NULL_OBJ) ray_retain(filter);
            return ray_i64(t->next_seq);
        }
    }

    if (t->n_subs >= t->cap_subs) {
        int32_t new_cap = t->cap_subs ? t->cap_subs * 2 : 8;
        ray_mcast_sub_t* ns = (ray_mcast_sub_t*)ray_sys_alloc(
            (size_t)new_cap * sizeof(ray_mcast_sub_t));
        if (!ns) return ray_error("oom", NULL);
        if (t->subs)
            memcpy(ns, t->subs, (size_t)t->n_subs * sizeof(ray_mcast_sub_t));
        memset(ns + t->n_subs, 0,
               (size_t)(new_cap - t->n_subs) * sizeof(ray_mcast_sub_t));
        ray_sys_free(t->subs);
        t->subs = ns;
        t->cap_subs = new_cap;
    }

    ray_mcast_sub_t* s = &t->subs[t->n_subs++];
    s->handle = handle;
    s->filter = filter;
    s->last_sent_seq = 0;
    s->dropped = 0;
    if (filter != RAY_NULL_OBJ) ray_retain(filter);
    return ray_i64(t->next_seq);
}

ray_t* ray_mcast_unsub(ray_poll_t* poll, int64_t handle, ray_t* topic) {
    if (handle < 0) return ray_error("domain", ".mc.unsub requires an active IPC connection");
    int64_t sym = topic_sym(topic);
    if (sym < 0) return ray_error("type", ".mc.unsub topic must be a symbol or string");
    ray_mcast_t* mc = (ray_mcast_t*)(poll ? poll->mcast : NULL);
    ray_mcast_topic_t* t = find_topic(mc, sym);
    if (!t) return RAY_NULL_OBJ;
    for (int32_t i = 0; i < t->n_subs; i++) {
        if (t->subs[i].handle == handle) {
            remove_sub(t, i);
            break;
        }
    }
    return RAY_NULL_OBJ;
}

ray_t* ray_mcast_pub(ray_poll_t* poll, ray_t* topic, ray_t* payload) {
    if (!poll) return ray_error("domain", ".mc.pub requires an active poll");
    int64_t sym = topic_sym(topic);
    if (sym < 0) return ray_error("type", ".mc.pub topic must be a symbol or string");
    if (ray_serde_size(payload) <= 0)
        return ray_error("type", ".mc.pub payload is not serializable");

    ray_mcast_t* mc = poll_mcast(poll);
    if (!mc) return ray_error("oom", NULL);
    ray_mcast_topic_t* t = ensure_topic(mc, sym);
    if (!t) return ray_error("oom", NULL);

    int64_t seq = t->next_seq++;
    mc->published++;

    for (int32_t i = 0; i < t->n_subs;) {
        ray_t* msg = make_upd_msg(sym, seq, payload);
        if (!msg || RAY_IS_ERR(msg)) return msg ? msg : ray_error("oom", NULL);
        ray_err_t rc = ray_ipc_send_async(t->subs[i].handle, msg);
        ray_release(msg);
        if (rc == RAY_OK) {
            t->subs[i].last_sent_seq = seq;
            mc->delivered++;
            i++;
        } else {
            t->subs[i].dropped++;
            mc->dropped++;
            remove_sub(t, i);
        }
    }

    return ray_i64(seq);
}

ray_t* ray_mcast_stats(ray_poll_t* poll) {
    ray_mcast_t* mc = (ray_mcast_t*)(poll ? poll->mcast : NULL);
    int64_t topics = mc ? mc->n_topics : 0;
    int64_t subs = 0;
    if (mc) {
        for (int32_t i = 0; i < mc->n_topics; i++)
            subs += mc->topics[i].n_subs;
    }

    ray_t* keys = ray_sym_vec_new(RAY_SYM_W64, 5);
    ray_t* vals = ray_list_new(5);
    if (!keys || RAY_IS_ERR(keys) || !vals || RAY_IS_ERR(vals)) {
        if (keys && !RAY_IS_ERR(keys)) ray_release(keys);
        if (vals && !RAY_IS_ERR(vals)) ray_release(vals);
        return ray_error("oom", NULL);
    }

    int64_t k;
    ray_t* v;
    k = ray_sym_intern("topics", 6);      keys = ray_vec_append(keys, &k);
    if (RAY_IS_ERR(keys)) { ray_release(vals); return keys; }
    v = ray_i64(topics);                  vals = ray_list_append(vals, v); ray_release(v);
    if (RAY_IS_ERR(vals)) { ray_release(keys); return vals; }
    k = ray_sym_intern("subscriptions", 13); keys = ray_vec_append(keys, &k);
    if (RAY_IS_ERR(keys)) { ray_release(vals); return keys; }
    v = ray_i64(subs);                    vals = ray_list_append(vals, v); ray_release(v);
    if (RAY_IS_ERR(vals)) { ray_release(keys); return vals; }
    k = ray_sym_intern("published", 9);   keys = ray_vec_append(keys, &k);
    if (RAY_IS_ERR(keys)) { ray_release(vals); return keys; }
    v = ray_i64(mc ? mc->published : 0);  vals = ray_list_append(vals, v); ray_release(v);
    if (RAY_IS_ERR(vals)) { ray_release(keys); return vals; }
    k = ray_sym_intern("delivered", 9);   keys = ray_vec_append(keys, &k);
    if (RAY_IS_ERR(keys)) { ray_release(vals); return keys; }
    v = ray_i64(mc ? mc->delivered : 0);  vals = ray_list_append(vals, v); ray_release(v);
    if (RAY_IS_ERR(vals)) { ray_release(keys); return vals; }
    k = ray_sym_intern("dropped", 7);     keys = ray_vec_append(keys, &k);
    if (RAY_IS_ERR(keys)) { ray_release(vals); return keys; }
    v = ray_i64(mc ? mc->dropped : 0);    vals = ray_list_append(vals, v); ray_release(v);
    if (RAY_IS_ERR(vals)) { ray_release(keys); return vals; }
    return ray_dict_new(keys, vals);
}

ray_t* ray_mcast_drop(ray_poll_t* poll, int64_t handle) {
    if (!poll) return ray_error("domain", ".mc.drop requires an active poll");
    if (handle < 0) return ray_error("domain", ".mc.drop handle must be >= 0");
    ray_mcast_on_close(poll, handle);
    ray_ipc_close(handle);
    return RAY_NULL_OBJ;
}

void ray_mcast_on_close(ray_poll_t* poll, int64_t handle) {
    ray_mcast_t* mc = (ray_mcast_t*)(poll ? poll->mcast : NULL);
    if (!mc || handle < 0) return;
    for (int32_t i = 0; i < mc->n_topics; i++) {
        ray_mcast_topic_t* t = &mc->topics[i];
        for (int32_t j = 0; j < t->n_subs;) {
            if (t->subs[j].handle == handle) remove_sub(t, j);
            else j++;
        }
    }
}
