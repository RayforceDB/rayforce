# Rayforce Multicast Usage Guide

This document describes how to use the current Rayforce multicast module and how to migrate existing IPC fan-out or legacy topic pub/sub code to it.

## Status

The current implementation is a live IPC multicast module exposed through `.mc.*`.

It supports:

- topic subscription over existing Rayforce IPC connections
- server-side fan-out with async `upd` delivery
- unsubscribe and automatic cleanup on IPC close
- string and symbol topic inputs
- serializable Rayforce payloads
- aggregate runtime statistics
- restricted-mode guard: restricted clients can subscribe but cannot publish

It does not yet provide durable replay, topic filters, wildcard topics, ACLs, per-topic stats, per-client lag, or queued backpressure.

## API

```clj
(.mc.sub topic null)       ;; subscribe the current IPC handle to topic
(.mc.unsub topic)          ;; unsubscribe the current IPC handle from topic
(.mc.pub topic payload)    ;; publish payload to all current subscribers
(.mc.stats)                ;; return aggregate multicast stats
```

`topic` can be a string or a symbol:

```clj
(.mc.sub "ticks" null)
(.mc.sub 'ticks null)

(.mc.pub "ticks" 42)
(.mc.pub 'ticks (dict ['sym 'price] (list 'AAPL 185.5)))
```

Filters are reserved for later work. For now, pass `null`:

```clj
(.mc.sub "ticks" null)     ;; ok
(.mc.sub "ticks" 1)        ;; nyi
```

## Delivery Model

Subscribers receive async IPC messages that evaluate this expression in the client process:

```clj
(upd topic seq payload)
```

The client must define `upd` before subscribing:

```clj
(set upd (fn [topic seq payload]
  (do
    (println "topic=% seq=% payload=%" topic seq payload)
    null)))
```

The delivered `topic` is a string even if the publisher used a symbol. This avoids Rayfall symbol name-resolution surprises in pushed expression-list messages.

The delivered `payload` is wrapped with `quote` on the wire, so lists, dicts, symbols, and other serializable values arrive as data rather than being evaluated as code by the subscriber.

`seq` is a per-topic publish sequence number. It is currently useful for logging and gap detection, but there is no replay API yet.

## Running A Server

Start a Rayforce IPC server:

```sh
./rayforce -p 5000
```

For server-only mode:

```sh
./rayforce -p 5000 </dev/null >rayforce.log 2>&1 &
```

Optionally load init code first:

```sh
./rayforce -p 5000 init.rfl
```

## Minimal Subscriber

```clj
(set count 0)

(set upd (fn [topic seq payload]
  (do
    (set count (+ count 1))
    (println "recv topic=% seq=% payload=% count=%" topic seq payload count)
    null)))

(set h (.ipc.open "127.0.0.1:5000" 2000))
(.ipc.send h "(.mc.sub \"ticks\" null)")
```

## Minimal Publisher

```clj
(set h (.ipc.open "127.0.0.1:5000" 2000))

(.ipc.send h "(.mc.pub \"ticks\" 123)")
(.ipc.send h "(.mc.pub \"ticks\" (dict ['sym 'price] (list 'AAPL 185.5)))")
```

## Stats

Use `.mc.stats` from any IPC client:

```clj
(.ipc.send h "(.mc.stats)")
```

Example result:

```clj
{topics:1 subscriptions:1 published:2 delivered:2 dropped:0}
```

Fields:

- `topics`: number of topic records with active subscriptions
- `subscriptions`: number of active topic subscriptions
- `published`: number of publish calls accepted by the module
- `delivered`: number of async messages successfully queued/sent to subscribers
- `dropped`: number of failed subscriber deliveries that caused subscriber removal

Stats are process-lifetime aggregate state. If several tests or clients reuse one server process, stats include earlier activity.

## Migrating Existing IPC Fan-Out

If existing code manually tracks handles and sends to each client:

```clj
;; old shape
(map (fn [h] (.ipc.post h msg)) clients)
```

migrate it as follows.

First, define `upd` in each client:

```clj
(set upd (fn [topic seq payload]
  (handle-event topic seq payload)))
```

Then subscribe each client to the topics it needs:

```clj
(.ipc.send h "(.mc.sub \"ticks\" null)")
(.ipc.send h "(.mc.sub \"orders\" null)")
```

Finally, replace producer-side handle fan-out with one publish call:

```clj
(.mc.pub "ticks" payload)
```

If the producer itself is remote, publish through IPC:

```clj
(.ipc.send h "(.mc.pub \"ticks\" payload_expr)")
```

## Migrating From Legacy Topic Pub/Sub

The shape is intentionally close to traditional ticker-plant style systems:

```text
sub(topic)         -> (.mc.sub "topic" null)
unsub(topic)       -> (.mc.unsub "topic")
pub(topic, data)   -> (.mc.pub "topic" data)
upd(topic, data)   -> upd(topic, seq, payload)
```

Main differences:

- Rayforce `upd` receives three arguments: `topic`, `seq`, and `payload`.
- `topic` is delivered as a string.
- `seq` is assigned by the server per topic.
- filters and replay are not implemented yet.

## Recommended Payload Shape

Prefer self-describing dict payloads for application events:

```clj
(dict ['sym 'ts 'price 'size]
      (list 'AAPL 2026.09.03D10:00:00.000000000 185.5 100))
```

For topic dispatch, keep `upd` small and delegate to topic-specific handlers:

```clj
(set onTick (fn [x] ...))
(set onOrder (fn [x] ...))

(set upd (fn [topic seq payload]
  (if (== topic "ticks")
      (onTick payload)
      (if (== topic "orders")
          (onOrder payload)
          null))))
```

## Operational Notes

- `.mc.sub` must run in an inbound IPC evaluation context because it uses the current IPC handle.
- Local `.mc.sub` outside IPC returns `domain`.
- `.mc.pub` returns the assigned sequence number.
- `.mc.pub` returns `0` when there is no active subscriber for the topic; it does not create an orphan topic.
- Duplicate subscription of the same handle to the same topic is idempotent.
- If async delivery to a subscriber would block or fails, that subscriber is removed, its IPC handle is closed, and `dropped` increments.
- Closing an IPC handle removes its subscriptions.
- Restricted IPC clients can subscribe but cannot publish.

## Real Test Scenarios

Manual end-to-end scenarios live under `test/scenarios/` and require a separately started IPC server.

Example:

```sh
./rayforce -p 5000 </dev/null >rayforce.log 2>&1 &
./rayforce test/scenarios/mcast_real_e2e.rfl -- --addr 127.0.0.1:5000
```

Available scenarios:

- `test/scenarios/mcast_real_e2e.rfl`: two clients, fan-out, unsubscribe, stats
- `test/scenarios/mcast_real_payloads.rfl`: i64, vector, symbol, and dict payloads
- `test/scenarios/mcast_real_negative.rfl`: publish without subscribers and negative API cases

The regular C regression suite can be run with:

```sh
make test TEST_FILTER=mcast
```
