# Rayforce Multicast Implementation Plan

## Goal

Add a first-class multicast/pub-sub subsystem to Rayforce as a core engine module, exposed to Rayfall through a `.mc.*` namespace and delivered over the existing IPC wire.

The target model is close to kdb tickerplant pub/sub, but adapted to Rayforce's current architecture:

- IPC handles are poll selector ids.
- Server-side code can push to connected clients through the same handle namespace.
- `.ipc.post` already provides async frames, but currently writes directly to the socket.
- The poll loop is single-threaded for IPC dispatch.
- Durable replay already exists through the journal layer.

The implementation should avoid UDP multicast initially. Rayforce needs typed payloads, authentication/restricted-mode behavior, replay, and per-client filtering; those fit application-level multicast over IPC much better than network-level multicast.

## Proposed User API

Expose multicast under reserved namespace `.mc.*`.

```lisp
(.mc.sub topic filter)
(.mc.unsub topic)
(.mc.pub topic payload)
(.mc.replay topic from-seq)
(.mc.stats)
(.mc.drop handle)
```

Initial semantics:

- `topic`: symbol or string, internally canonicalized to a symbol id or stable string key.
- `filter`: null, symbol vector/list, dict, or lambda. MVP should support only null and simple dict filters; lambdas can be a later phase.
- `payload`: any serializable Rayforce value, normally a table, vector, dict, or list.
- `.mc.pub` returns the assigned sequence number.
- `.mc.sub` uses `(.ipc.handle)` when called during inbound IPC evaluation. Optionally support explicit handle form later.
- pushed client message is an async IPC frame containing:

```lisp
(upd topic seq payload)
```

This mirrors the kdb convention where downstream clients implement `upd`. In
Rayforce the delivered `topic` should be a string, not a bare symbol, because
unquoted symbol atoms in expression position are name references in Rayfall.
Symbol payload atoms must be sent as quoted/literal symbols for the same reason.

## Module Boundary

Add a static core module, not a dynamic plugin ABI.

Files:

- `src/core/mcast.h`
- `src/core/mcast.c`
- `src/ops/mcast.c` for Rayfall builtin wrappers, or place the wrappers beside the existing IPC builtins in `src/ops/system.c` if that is still the local convention.
- `test/test_mcast.c`
- `docs/docs/namespaces/mc.md`
- update `docs/docs/namespaces/index.md`

Suggested C API:

```c
typedef struct ray_mcast ray_mcast_t;

ray_mcast_t* ray_mcast_create(void);
void         ray_mcast_destroy(ray_mcast_t* mc);

ray_t*       ray_mcast_sub(ray_poll_t* poll, int64_t handle,
                           ray_t* topic, ray_t* filter);
ray_t*       ray_mcast_unsub(ray_poll_t* poll, int64_t handle,
                             ray_t* topic);
ray_t*       ray_mcast_pub(ray_poll_t* poll, ray_t* topic, ray_t* payload);
ray_t*       ray_mcast_replay(ray_poll_t* poll, ray_t* topic,
                              int64_t from_seq);
ray_t*       ray_mcast_stats(ray_poll_t* poll);
void         ray_mcast_on_close(ray_poll_t* poll, int64_t handle);
```

Store the module instance on `ray_poll_t` as an opaque pointer:

```c
void* mcast;
```

Reason: Rayforce IPC handles are poll-local selector ids, so multicast subscriptions and outbound queues naturally belong to the poll that owns the connections.

## Internal Data Model

Core structures:

```c
typedef struct ray_mcast_sub {
    int64_t handle;
    ray_t*  filter;
    int64_t last_sent_seq;
    int64_t dropped;
} ray_mcast_sub_t;

typedef struct ray_mcast_topic {
    int64_t topic_sym;
    int64_t next_seq;
    ray_mcast_sub_t* subs;
    int32_t n_subs;
    int32_t cap_subs;
} ray_mcast_topic_t;

typedef struct ray_mcast {
    ray_mcast_topic_t* topics;
    int32_t n_topics;
    int32_t cap_topics;
    int64_t published;
    int64_t delivered;
    int64_t dropped;
} ray_mcast_t;
```

MVP can use linear arrays, matching the current small fixed/simple style in the runtime. Replace with hash tables only when subscriber/topic counts make it necessary.

Memory ownership:

- retain `filter` on subscribe;
- release `filter` on unsubscribe/close/destroy;
- publish payload should be retained only while building queued outbound frames;
- avoid storing published payloads in memory unless replay cache is explicitly implemented.

## Delivery Path

Phase 1 can call the existing async IPC send path, but the production design should introduce queued transmit.

Current issue:

- `ray_ipc_send_async` resolves a handle and immediately calls the socket write path.
- That can block the publish path on slow subscribers or large payloads.

Required engine improvement:

1. Split IPC frame creation from socket writing.
2. Add per-selector TX queue.
3. Teach poll backends to watch socket writability when TX queue is non-empty.
4. Let multicast enqueue frames to each subscriber and return quickly.
5. Apply per-connection queue limits.

Suggested helper boundary:

```c
ray_err_t ray_ipc_enqueue_async(ray_poll_t* poll, int64_t handle, ray_t* msg);
ray_err_t ray_ipc_enqueue_frame(ray_poll_t* poll, int64_t handle,
                                uint8_t msgtype, ray_t* msg,
                                uint8_t extra_flags);
```

`ray_ipc_send_async` can then become a wrapper over enqueue for poll-owned connections.

## Backpressure Policy

Make slow-client behavior explicit per topic or process.

Policies:

- `disconnect`: default for event streams where loss is unacceptable.
- `drop-oldest`: useful for telemetry and snapshot-like topics.
- `drop-latest`: useful when preserving queued order matters more than newest value.
- `coalesce`: later optimization for keyed state topics, e.g. latest quote per sym.

MVP default:

- bounded queue per connection;
- if full, disconnect subscriber and increment dropped/slow counters.

This is stricter than silently dropping, and easier to reason about for market/event data.

## Filtering

Start narrow.

Phase 1 filters:

- null filter: receive every message for the topic;
- dict filter for common table payload columns, e.g. `{sym: [AAPL MSFT]}`;
- symbol/list filter shorthand can map to `{sym: ...}` only if we explicitly document that convention.

Avoid lambda filters in the first implementation. Calling Rayfall lambdas per subscriber inside publish adds eval reentrancy, no-closure edge cases, unpredictable latency, and error semantics that are not needed for the first useful version.

For table payloads, filtering should create a selected table per subscriber only if needed. If multiple subscribers have the same filter, later optimization can cache the filtered payload for that publish call.

## Durability And Replay

There are two useful stream classes:

- state streams: quotes, metrics, book snapshots; loss can be repaired by a fresh snapshot;
- event streams: trades, orders, audit; loss requires replay or disconnect.

MVP:

- assign sequence numbers per topic;
- `.mc.pub` writes a journal expression before fanout if multicast journaling is enabled;
- `.mc.replay` can initially return an explicit `nyi` until journal indexing is designed.

Durable phase:

- journal each publish as `(list '.mc.apply topic seq payload)`;
- on recovery, rebuild topic sequence counters and optional retained state;
- add a compact side index by topic/seq if replay from large logs matters.

Do not rely on the generic journal alone for efficient replay. It can reconstruct state, but topic-range replay needs indexing or a retained log table.

## Restricted Mode And Security

Recommended builtin flags:

- `.mc.sub`: allowed in restricted IPC mode.
- `.mc.unsub`: allowed in restricted IPC mode.
- `.mc.stats`: allowed, unless it leaks sensitive details.
- `.mc.pub`: restricted by default.
- `.mc.drop`: restricted.
- `.mc.replay`: allowed only if it returns data the caller is already authorized to receive.

Auth should remain at IPC handshake level. Fine-grained authorization can be added through `.ipc.on.auth` or a future `.mc.auth` policy hook, but do not add that in MVP.

## Lifecycle Integration

1. Initialize `poll->mcast` lazily on first `.mc.*` call.
2. Destroy it in `ray_poll_destroy`.
3. Call `ray_mcast_on_close(poll, sel->id)` from IPC close handling for inbound and outbound IPC connections.
4. Ensure selector id reuse cannot leak subscriptions: close cleanup must run before the selector slot can be reused.
5. If listener teardown destroys all selectors, multicast cleanup should release every retained filter and queued message.

## Implementation Phases

### Phase 0: Design Lock

- Add this plan to the repo.
- Decide exact names: `.mc.*` vs `.ipc.pubsub.*`.
- Decide whether topics are symbols only or symbol/string.
- Decide whether `.mc.sub` takes explicit handle in addition to current IPC handle.

Recommended decisions:

- use `.mc.*`;
- accept symbol or string but canonicalize internally;
- MVP uses current IPC handle only; add explicit admin forms later.

### Phase 1: Minimal Core Multicast

- Add `src/core/mcast.h`.
- Add `src/core/mcast.c`.
- Add `void* mcast` to `ray_poll_t`.
- Add create/destroy/lazy-get helpers.
- Implement topic lookup, subscribe, unsubscribe, close cleanup, stats.
- Implement publish using current `ray_ipc_send_async` or a small internal enqueue placeholder if TX queue work is started immediately.
- Register `.mc.sub`, `.mc.unsub`, `.mc.pub`, `.mc.stats`.
- Add docs page with MVP semantics and limitations.

Tests:

- subscribe from an IPC client;
- publish delivers `(upd topic seq payload)` to that client;
- two clients receive the same message;
- unsubscribe stops delivery;
- close removes subscriptions;
- `.mc.stats` reports topic/subscriber counts.

### Phase 2: TX Queue And Nonblocking Fanout

- Refactor IPC frame serialization into reusable frame builder.
- Add TX queue to `ray_selector`.
- Add poll backend write readiness support for Linux epoll and macOS kqueue.
- Change `.ipc.post` to enqueue instead of direct socket write where possible.
- Add queue byte/message limits.
- Add disconnect-on-overflow policy.

Tests:

- large payload fanout to multiple subscribers does not block publish on the first slow reader;
- queue overflow disconnects or drops according to configured policy;
- queued frames preserve order on a single connection;
- sync `.ipc.send` still pumps inbound pushed async frames correctly.

### Phase 3: Filters

- Implement null filter.
- Implement dict equality/inclusion filter for table payloads.
- Implement topic-level filter validation at subscribe time.
- Return clear `type`/`domain` errors for unsupported filters.

Tests:

- `{sym: [AAPL MSFT]}` receives matching rows only;
- non-table payload with table-column filter errors or is skipped according to documented policy;
- multiple subscribers with different filters get different payloads.

### Phase 4: Durability And Replay

- Add `.mc.config` or process-level config for durable topics.
- Journal publish records before fanout.
- Implement replay API from retained in-memory log or journal side index.
- Add startup recovery behavior for sequence counters.

Tests:

- after replay/restart, next sequence continues after last published seq;
- replay from seq returns expected messages;
- corrupted tail follows existing journal validation behavior.

### Phase 5: Operations Surface

- Add `.mc.topics`.
- Add `.mc.subs topic`.
- Add `.mc.config` for queue limits and policy.
- Add querylog/profile integration if needed.
- Add documentation examples for tickerplant-style market data.

## Test Strategy

Add `test/test_mcast.c` using the existing IPC test style:

- create runtime and poll;
- start poll server thread;
- connect clients with `ray_ipc_connect`;
- install client-side `.ipc.on.async` or use pushed eval side effects as current tests do;
- publish from server-side eval or direct C API;
- use sync round-trip as an ordering barrier.

Also add Rayfall integration tests under `test/rfl/system/` once the `.mc.*` namespace exists.

Regression areas:

- selector id reuse after close;
- restricted mode access;
- non-serializable payloads;
- publish from inside `.ipc.on.sync`;
- publish while a client is already in sync wait;
- large compressed payloads;
- cleanup on poll destroy.

## Open Questions

- Should `.mc.pub` be allowed locally only, or also from authenticated unrestricted IPC clients?
- Should sequence numbers be global or per topic? Recommendation: per topic.
- Should the pushed function name be fixed as `upd`, or configurable per subscription? Recommendation: fixed first.
- Should filters be applied before journaling or after? Recommendation: journal original payload, filter only for delivery.
- Should replay use existing `.log` files or a dedicated multicast log table? Recommendation: use existing journal for recovery, add a topic/seq index for replay.

## Initial Acceptance Criteria

The first merge is useful if it satisfies:

- a Rayforce process can accept two IPC clients;
- both clients can call `.mc.sub`;
- server can call `.mc.pub`;
- both clients receive async `(upd topic seq payload)` messages in order;
- closing one client removes only that client's subscriptions;
- restricted clients can subscribe but cannot publish;
- tests cover lifecycle, fanout, and close cleanup.
