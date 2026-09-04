/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 */

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#define _GNU_SOURCE

#include "test.h"
#include <rayforce.h>
#include "core/ipc.h"
#include "core/poll.h"
#include "core/runtime.h"
#include "core/sock.h"
#include "lang/env.h"
#include "table/dict.h"
#include "mem/sys.h"

#ifndef RAY_OS_WINDOWS
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <unistd.h>
#endif

#include <string.h>
#include <time.h>

extern ray_runtime_t* __RUNTIME;

typedef struct {
    ray_poll_t* poll;
    ray_vm_t*   vm;
} poll_thread_ctx_t;

static poll_thread_ctx_t g_server_ctx;

static void mcast_setup(void) {
    ray_runtime_create(0, NULL);
    ray_poll_t* p = ray_poll_create();
    if (p) ray_runtime_set_poll(p);
}

static void mcast_teardown(void) {
    ray_poll_t* p = (ray_poll_t*)ray_runtime_get_poll();
    if (p) {
        ray_runtime_set_poll(NULL);
        ray_poll_destroy(p);
    }
    ray_runtime_destroy(__RUNTIME);
}

static void sleep_ms(long ms) {
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void pump_client(void) {
    ray_poll_t* poll = (ray_poll_t*)ray_runtime_get_poll();
    if (!poll) return;
    for (int i = 0; i < 20; i++) {
        ray_poll_run_for(poll, 10);
        sleep_ms(1);
    }
}

static uint16_t get_listen_port(ray_sock_t fd) {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    if (getsockname(fd, (struct sockaddr*)&addr, &len) < 0) return 0;
    return ntohs(addr.sin_port);
}

static ray_vm_t* make_server_vm(void) {
    ray_vm_t* vm = (ray_vm_t*)ray_sys_alloc(sizeof(ray_vm_t));
    if (!vm) return NULL;
    ray_vm_init(vm, 77);
    return vm;
}

static void poll_server_thread_fn(void* arg) {
    poll_thread_ctx_t* ctx = (poll_thread_ctx_t*)arg;
    __VM = ctx->vm;
    ray_poll_run(ctx->poll);
}

static void poll_stop(ray_poll_t* poll, uint16_t port) {
    ray_poll_exit(poll, 0);
    ray_sock_t k = ray_sock_connect("127.0.0.1", port, 200);
    if (k != RAY_INVALID_SOCK) ray_sock_close(k);
}

static int64_t dict_i64(ray_t* d, const char* key) {
    ray_t* k = ray_sym(ray_sym_intern(key, strlen(key)));
    if (!k || RAY_IS_ERR(k)) return -1;
    ray_t* v = ray_dict_get(d, k);
    ray_release(k);
    if (!v) return -1;
    int64_t out = (v->type == -RAY_I64) ? v->i64 : -1;
    ray_release(v);
    return out;
}

static test_result_t assert_ok(ray_t* x, const char* label) {
    TEST_ASSERT_NOT_NULL(x);
    if (RAY_IS_ERR(x)) {
        const char* code = ray_err_code(x);
        TEST_ASSERT_FMT(false, "%s returned error: %s", label, code ? code : "error");
    }
    PASS();
}

static test_result_t start_server(ray_poll_t** poll_out, uint16_t* port_out,
                                  ray_vm_t** vm_out, ray_thread_t* tid_out) {
    ray_poll_t* poll = ray_poll_create();
    TEST_ASSERT_NOT_NULL(poll);
    int64_t listener_id = ray_ipc_listen(poll, 0);
    TEST_ASSERT((listener_id) >= (0), "listener_id >= 0");
    ray_selector_t* listener_sel = ray_poll_get(poll, listener_id);
    TEST_ASSERT_NOT_NULL(listener_sel);
    uint16_t port = get_listen_port((ray_sock_t)listener_sel->fd);
    TEST_ASSERT((port) > (0), "port > 0");

    ray_vm_t* vm = make_server_vm();
    TEST_ASSERT_NOT_NULL(vm);
    g_server_ctx.poll = poll;
    g_server_ctx.vm = vm;
    ray_thread_create(tid_out, poll_server_thread_fn, &g_server_ctx);
    sleep_ms(20);

    *poll_out = poll;
    *port_out = port;
    *vm_out = vm;
    PASS();
}

static void stop_server(ray_poll_t* poll, uint16_t port, ray_vm_t* vm, ray_thread_t tid) {
    poll_stop(poll, port);
    ray_thread_join(tid);
    ray_poll_destroy(poll);
    ray_sys_free(vm);
}

static test_result_t test_mcast_single_client_pub(void) {
    ray_t* r = ray_eval_str(
        "(set _mc_count 0)"
        "(set _mc_last_seq 0)"
        "(set _mc_last_topic \"\")"
        "(set _mc_last_payload 0)"
        "(set upd (fn [topic seq payload] "
        "  (set _mc_count (+ _mc_count 1))"
        "  (set _mc_last_seq seq)"
        "  (set _mc_last_topic topic)"
        "  (set _mc_last_payload payload)))");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    if (r != RAY_NULL_OBJ) ray_release(r);

    ray_poll_t* poll;
    ray_vm_t* vm;
    uint16_t port;
    ray_thread_t tid;
    test_result_t sr = start_server(&poll, &port, &vm, &tid);
    if (sr.status != TEST_PASS) return sr;

    int64_t h = ray_ipc_connect("127.0.0.1", port, NULL, NULL, 0);
    TEST_ASSERT((h) >= (0), "client connected");

    ray_t* sub_msg = ray_str("(.mc.sub \"ticks\" null)", strlen("(.mc.sub \"ticks\" null)"));
    ray_t* sub = ray_ipc_send(h, sub_msg);
    ray_release(sub_msg);
    test_result_t ok = assert_ok(sub, "sub");
    if (ok.status != TEST_PASS) return ok;
    TEST_ASSERT_EQ_I(sub->i64, 1);
    ray_release(sub);

    ray_t* pub_msg = ray_str("(.mc.pub \"ticks\" 42)", strlen("(.mc.pub \"ticks\" 42)"));
    ray_t* pub = ray_ipc_send(h, pub_msg);
    ray_release(pub_msg);
    ok = assert_ok(pub, "pub");
    if (ok.status != TEST_PASS) return ok;
    TEST_ASSERT_EQ_I(pub->i64, 1);
    ray_release(pub);
    pump_client();

    ray_t* c = ray_env_get(ray_sym_intern("_mc_count", 9));
    ray_t* s = ray_env_get(ray_sym_intern("_mc_last_seq", 12));
    ray_t* t = ray_env_get(ray_sym_intern("_mc_last_topic", 14));
    ray_t* p = ray_env_get(ray_sym_intern("_mc_last_payload", 16));
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQ_I(c->i64, 1);
    TEST_ASSERT_EQ_I(s->i64, 1);
    TEST_ASSERT_EQ_I(t->type, -RAY_STR);
    TEST_ASSERT_STR_EQ(ray_str_ptr(t), "ticks");
    TEST_ASSERT_EQ_I(p->i64, 42);

    pub_msg = ray_str("(.mc.pub \"ticks\" 'IBM)", strlen("(.mc.pub \"ticks\" 'IBM)"));
    pub = ray_ipc_send(h, pub_msg);
    ray_release(pub_msg);
    ok = assert_ok(pub, "pub symbol");
    if (ok.status != TEST_PASS) return ok;
    TEST_ASSERT_EQ_I(pub->i64, 2);
    ray_release(pub);
    pump_client();

    c = ray_env_get(ray_sym_intern("_mc_count", 9));
    s = ray_env_get(ray_sym_intern("_mc_last_seq", 12));
    p = ray_env_get(ray_sym_intern("_mc_last_payload", 16));
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQ_I(c->i64, 2);
    TEST_ASSERT_EQ_I(s->i64, 2);
    TEST_ASSERT_EQ_I(p->type, -RAY_SYM);
    ray_t* ps = ray_sym_str(p->i64);
    TEST_ASSERT_NOT_NULL(ps);
    TEST_ASSERT_STR_EQ(ray_str_ptr(ps), "IBM");

    ray_ipc_close(h);
    stop_server(poll, port, vm, tid);
    PASS();
}

static test_result_t test_mcast_two_clients_unsub_close_stats(void) {
    ray_t* r = ray_eval_str(
        "(set _mc_count 0)"
        "(set upd (fn [topic seq payload] (set _mc_count (+ _mc_count payload))))");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    if (r != RAY_NULL_OBJ) ray_release(r);

    ray_poll_t* poll;
    ray_vm_t* vm;
    uint16_t port;
    ray_thread_t tid;
    test_result_t sr = start_server(&poll, &port, &vm, &tid);
    if (sr.status != TEST_PASS) return sr;

    int64_t h1 = ray_ipc_connect("127.0.0.1", port, NULL, NULL, 0);
    int64_t h2 = ray_ipc_connect("127.0.0.1", port, NULL, NULL, 0);
    TEST_ASSERT((h1) >= (0), "h1 connected");
    TEST_ASSERT((h2) >= (0), "h2 connected");

    ray_t* msg = ray_str("(.mc.sub \"ticks\" null)", strlen("(.mc.sub \"ticks\" null)"));
    ray_t* r1 = ray_ipc_send(h1, msg);
    ray_t* r2 = ray_ipc_send(h2, msg);
    ray_release(msg);
    TEST_ASSERT_NOT_NULL(r1); TEST_ASSERT_FALSE(RAY_IS_ERR(r1)); ray_release(r1);
    TEST_ASSERT_NOT_NULL(r2); TEST_ASSERT_FALSE(RAY_IS_ERR(r2)); ray_release(r2);

    ray_t* st_msg = ray_str("(.mc.stats)", strlen("(.mc.stats)"));
    ray_t* st = ray_ipc_send(h1, st_msg);
    ray_release(st_msg);
    TEST_ASSERT_NOT_NULL(st);
    TEST_ASSERT_FALSE(RAY_IS_ERR(st));
    TEST_ASSERT_EQ_I(dict_i64(st, "topics"), 1);
    TEST_ASSERT_EQ_I(dict_i64(st, "subscriptions"), 2);
    ray_release(st);

    ray_t* unsub_msg = ray_str("(.mc.unsub \"ticks\")", strlen("(.mc.unsub \"ticks\")"));
    ray_t* ur = ray_ipc_send(h2, unsub_msg);
    ray_release(unsub_msg);
    TEST_ASSERT_NOT_NULL(ur);
    TEST_ASSERT_FALSE(RAY_IS_ERR(ur));
    ray_release(ur);

    ray_t* pub_msg = ray_str("(.mc.pub \"ticks\" 5)", strlen("(.mc.pub \"ticks\" 5)"));
    ray_t* pub = ray_ipc_send(h1, pub_msg);
    ray_release(pub_msg);
    TEST_ASSERT_NOT_NULL(pub);
    TEST_ASSERT_FALSE(RAY_IS_ERR(pub));
    ray_release(pub);
    pump_client();

    ray_t* barrier_msg = ray_str("(+ 0 0)", strlen("(+ 0 0)"));
    ray_t* barrier = ray_ipc_send(h2, barrier_msg);
    ray_release(barrier_msg);
    TEST_ASSERT_NOT_NULL(barrier);
    TEST_ASSERT_FALSE(RAY_IS_ERR(barrier));
    ray_release(barrier);

    ray_t* c = ray_env_get(ray_sym_intern("_mc_count", 9));
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQ_I(c->i64, 5);

    msg = ray_str("(.mc.sub \"ticks\" null)", strlen("(.mc.sub \"ticks\" null)"));
    r2 = ray_ipc_send(h2, msg);
    ray_release(msg);
    TEST_ASSERT_NOT_NULL(r2); TEST_ASSERT_FALSE(RAY_IS_ERR(r2)); ray_release(r2);
    ray_ipc_close(h2);
    sleep_ms(50);

    st_msg = ray_str("(.mc.stats)", strlen("(.mc.stats)"));
    st = ray_ipc_send(h1, st_msg);
    ray_release(st_msg);
    TEST_ASSERT_NOT_NULL(st);
    TEST_ASSERT_FALSE(RAY_IS_ERR(st));
    TEST_ASSERT_EQ_I(dict_i64(st, "subscriptions"), 1);
    ray_release(st);

    ray_ipc_close(h1);
    stop_server(poll, port, vm, tid);
    PASS();
}

static test_result_t test_mcast_duplicate_sub_is_idempotent(void) {
    ray_t* r = ray_eval_str(
        "(set _mc_count 0)"
        "(set upd (fn [topic seq payload] (set _mc_count (+ _mc_count 1))))");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    if (r != RAY_NULL_OBJ) ray_release(r);

    ray_poll_t* poll;
    ray_vm_t* vm;
    uint16_t port;
    ray_thread_t tid;
    test_result_t sr = start_server(&poll, &port, &vm, &tid);
    if (sr.status != TEST_PASS) return sr;

    int64_t h = ray_ipc_connect("127.0.0.1", port, NULL, NULL, 0);
    TEST_ASSERT((h) >= (0), "client connected");

    ray_t* msg = ray_str("(.mc.sub \"dup\" null)", strlen("(.mc.sub \"dup\" null)"));
    ray_t* sub1 = ray_ipc_send(h, msg);
    ray_t* sub2 = ray_ipc_send(h, msg);
    ray_release(msg);
    TEST_ASSERT_NOT_NULL(sub1); TEST_ASSERT_FALSE(RAY_IS_ERR(sub1)); ray_release(sub1);
    TEST_ASSERT_NOT_NULL(sub2); TEST_ASSERT_FALSE(RAY_IS_ERR(sub2)); ray_release(sub2);

    msg = ray_str("(.mc.stats)", strlen("(.mc.stats)"));
    ray_t* st = ray_ipc_send(h, msg);
    ray_release(msg);
    TEST_ASSERT_NOT_NULL(st);
    TEST_ASSERT_FALSE(RAY_IS_ERR(st));
    TEST_ASSERT_EQ_I(dict_i64(st, "topics"), 1);
    TEST_ASSERT_EQ_I(dict_i64(st, "subscriptions"), 1);
    ray_release(st);

    msg = ray_str("(.mc.pub \"dup\" 1)", strlen("(.mc.pub \"dup\" 1)"));
    ray_t* pub = ray_ipc_send(h, msg);
    ray_release(msg);
    TEST_ASSERT_NOT_NULL(pub);
    TEST_ASSERT_FALSE(RAY_IS_ERR(pub));
    TEST_ASSERT_EQ_I(pub->i64, 1);
    ray_release(pub);
    pump_client();

    ray_t* c = ray_env_get(ray_sym_intern("_mc_count", 9));
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQ_I(c->i64, 1);

    ray_ipc_close(h);
    stop_server(poll, port, vm, tid);
    PASS();
}

static test_result_t test_mcast_multi_topic_routing(void) {
    ray_t* r = ray_eval_str(
        "(set _mc_count 0)"
        "(set _mc_sum 0)"
        "(set _mc_last_topic \"\")"
        "(set upd (fn [topic seq payload] "
        "  (set _mc_count (+ _mc_count 1))"
        "  (set _mc_sum (+ _mc_sum payload))"
        "  (set _mc_last_topic topic)))");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    if (r != RAY_NULL_OBJ) ray_release(r);

    ray_poll_t* poll;
    ray_vm_t* vm;
    uint16_t port;
    ray_thread_t tid;
    test_result_t sr = start_server(&poll, &port, &vm, &tid);
    if (sr.status != TEST_PASS) return sr;

    int64_t h = ray_ipc_connect("127.0.0.1", port, NULL, NULL, 0);
    TEST_ASSERT((h) >= (0), "client connected");

    ray_t* msg = ray_str("(.mc.sub 'alpha null)", strlen("(.mc.sub 'alpha null)"));
    ray_t* sub = ray_ipc_send(h, msg);
    ray_release(msg);
    TEST_ASSERT_NOT_NULL(sub); TEST_ASSERT_FALSE(RAY_IS_ERR(sub)); ray_release(sub);

    msg = ray_str("(.mc.sub \"beta\" null)", strlen("(.mc.sub \"beta\" null)"));
    sub = ray_ipc_send(h, msg);
    ray_release(msg);
    TEST_ASSERT_NOT_NULL(sub); TEST_ASSERT_FALSE(RAY_IS_ERR(sub)); ray_release(sub);

    msg = ray_str("(.mc.pub \"alpha\" 11)", strlen("(.mc.pub \"alpha\" 11)"));
    ray_t* pub = ray_ipc_send(h, msg);
    ray_release(msg);
    TEST_ASSERT_NOT_NULL(pub); TEST_ASSERT_FALSE(RAY_IS_ERR(pub)); ray_release(pub);
    pump_client();

    msg = ray_str("(.mc.pub 'beta 13)", strlen("(.mc.pub 'beta 13)"));
    pub = ray_ipc_send(h, msg);
    ray_release(msg);
    TEST_ASSERT_NOT_NULL(pub); TEST_ASSERT_FALSE(RAY_IS_ERR(pub)); ray_release(pub);
    pump_client();

    ray_t* c = ray_env_get(ray_sym_intern("_mc_count", 9));
    ray_t* sum = ray_env_get(ray_sym_intern("_mc_sum", 7));
    ray_t* topic = ray_env_get(ray_sym_intern("_mc_last_topic", 14));
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_NOT_NULL(sum);
    TEST_ASSERT_NOT_NULL(topic);
    TEST_ASSERT_EQ_I(c->i64, 2);
    TEST_ASSERT_EQ_I(sum->i64, 24);
    TEST_ASSERT_EQ_I(topic->type, -RAY_STR);
    TEST_ASSERT_STR_EQ(ray_str_ptr(topic), "beta");

    msg = ray_str("(.mc.stats)", strlen("(.mc.stats)"));
    ray_t* st = ray_ipc_send(h, msg);
    ray_release(msg);
    TEST_ASSERT_NOT_NULL(st);
    TEST_ASSERT_FALSE(RAY_IS_ERR(st));
    TEST_ASSERT_EQ_I(dict_i64(st, "topics"), 2);
    TEST_ASSERT_EQ_I(dict_i64(st, "subscriptions"), 2);
    TEST_ASSERT_EQ_I(dict_i64(st, "published"), 2);
    TEST_ASSERT_EQ_I(dict_i64(st, "delivered"), 2);
    ray_release(st);

    ray_ipc_close(h);
    stop_server(poll, port, vm, tid);
    PASS();
}

static test_result_t test_mcast_no_subscribers_and_api_errors(void) {
    ray_poll_t* poll;
    ray_vm_t* vm;
    uint16_t port;
    ray_thread_t tid;
    test_result_t sr = start_server(&poll, &port, &vm, &tid);
    if (sr.status != TEST_PASS) return sr;

    int64_t h = ray_ipc_connect("127.0.0.1", port, NULL, NULL, 0);
    TEST_ASSERT((h) >= (0), "client connected");

    ray_t* msg = ray_str("(.mc.pub \"orphan\" 99)", strlen("(.mc.pub \"orphan\" 99)"));
    ray_t* pub = ray_ipc_send(h, msg);
    ray_release(msg);
    TEST_ASSERT_NOT_NULL(pub);
    TEST_ASSERT_FALSE(RAY_IS_ERR(pub));
    TEST_ASSERT_EQ_I(pub->i64, 1);
    ray_release(pub);

    msg = ray_str("(.mc.stats)", strlen("(.mc.stats)"));
    ray_t* st = ray_ipc_send(h, msg);
    ray_release(msg);
    TEST_ASSERT_NOT_NULL(st);
    TEST_ASSERT_FALSE(RAY_IS_ERR(st));
    TEST_ASSERT_EQ_I(dict_i64(st, "topics"), 1);
    TEST_ASSERT_EQ_I(dict_i64(st, "subscriptions"), 0);
    TEST_ASSERT_EQ_I(dict_i64(st, "published"), 1);
    TEST_ASSERT_EQ_I(dict_i64(st, "delivered"), 0);
    TEST_ASSERT_EQ_I(dict_i64(st, "dropped"), 0);
    ray_release(st);

    msg = ray_str("(.mc.sub \"bad-filter\" 1)", strlen("(.mc.sub \"bad-filter\" 1)"));
    ray_t* err = ray_ipc_send(h, msg);
    ray_release(msg);
    TEST_ASSERT_NOT_NULL(err);
    TEST_ASSERT_TRUE(RAY_IS_ERR(err));
    TEST_ASSERT_STR_EQ(ray_err_code(err), "nyi");
    ray_error_free(err);

    msg = ray_str("(.mc.pub [1 2] 1)", strlen("(.mc.pub [1 2] 1)"));
    err = ray_ipc_send(h, msg);
    ray_release(msg);
    TEST_ASSERT_NOT_NULL(err);
    TEST_ASSERT_TRUE(RAY_IS_ERR(err));
    TEST_ASSERT_STR_EQ(ray_err_code(err), "type");
    ray_error_free(err);

    msg = ray_str("(.mc.drop -1)", strlen("(.mc.drop -1)"));
    err = ray_ipc_send(h, msg);
    ray_release(msg);
    TEST_ASSERT_NOT_NULL(err);
    TEST_ASSERT_TRUE(RAY_IS_ERR(err));
    TEST_ASSERT_STR_EQ(ray_err_code(err), "domain");
    ray_error_free(err);

    ray_ipc_close(h);
    stop_server(poll, port, vm, tid);
    PASS();
}

static test_result_t test_mcast_restricted_sub_but_not_pub(void) {
    ray_poll_t* poll;
    ray_vm_t* vm;
    uint16_t port;
    ray_thread_t tid;
    test_result_t sr = start_server(&poll, &port, &vm, &tid);
    if (sr.status != TEST_PASS) return sr;
    strcpy(poll->auth_secret, "secret");
    ray_poll_set_restricted(poll, true);

    int64_t h = ray_ipc_connect("127.0.0.1", port, "u", "secret", 0);
    TEST_ASSERT((h) >= (0), "restricted client connected");

    ray_t* msg = ray_str("(.mc.sub \"ticks\" null)", strlen("(.mc.sub \"ticks\" null)"));
    ray_t* sub = ray_ipc_send(h, msg);
    ray_release(msg);
    TEST_ASSERT_NOT_NULL(sub);
    TEST_ASSERT_FALSE(RAY_IS_ERR(sub));
    ray_release(sub);

    msg = ray_str("(.mc.pub \"ticks\" 1)", strlen("(.mc.pub \"ticks\" 1)"));
    ray_t* pub = ray_ipc_send(h, msg);
    ray_release(msg);
    TEST_ASSERT_NOT_NULL(pub);
    TEST_ASSERT_TRUE(RAY_IS_ERR(pub));
    TEST_ASSERT_STR_EQ(ray_err_code(pub), "access");
    ray_error_free(pub);

    ray_ipc_close(h);
    stop_server(poll, port, vm, tid);
    PASS();
}

static test_result_t test_mcast_requires_ipc_context_for_sub(void) {
    ray_t* sub = ray_eval_str("(.mc.sub \"ticks\" null)");
    TEST_ASSERT_NOT_NULL(sub);
    TEST_ASSERT_TRUE(RAY_IS_ERR(sub));
    TEST_ASSERT_STR_EQ(ray_err_code(sub), "domain");
    ray_error_free(sub);
    PASS();
}

const test_entry_t mcast_entries[] = {
    { "mcast/single_client_pub",          test_mcast_single_client_pub,          mcast_setup, mcast_teardown },
    { "mcast/two_clients_unsub_close",    test_mcast_two_clients_unsub_close_stats, mcast_setup, mcast_teardown },
    { "mcast/duplicate_sub_idempotent",   test_mcast_duplicate_sub_is_idempotent, mcast_setup, mcast_teardown },
    { "mcast/multi_topic_routing",        test_mcast_multi_topic_routing,        mcast_setup, mcast_teardown },
    { "mcast/no_subscribers_api_errors",  test_mcast_no_subscribers_and_api_errors, mcast_setup, mcast_teardown },
    { "mcast/restricted_sub_not_pub",     test_mcast_restricted_sub_but_not_pub, mcast_setup, mcast_teardown },
    { "mcast/sub_requires_ipc_context",   test_mcast_requires_ipc_context_for_sub, mcast_setup, mcast_teardown },
    { NULL, NULL, NULL, NULL },
};
