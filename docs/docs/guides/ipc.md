# IPC Guide

Rayforce includes a built-in IPC layer that turns any Rayforce process into a query server. Clients connect over TCP, send queries or data, and receive serialized results back. This guide walks through the full workflow from starting a server to building multi-process architectures.

## 1. Getting Started

The fastest way to try IPC: start a server in one terminal and connect from another.

### Start a Server

The `-p` flag starts Rayforce in server mode, listening on the given port:

```bash
./rayforce -p 5000
```

The server starts, prints a listening message, and waits for connections. It also runs a REPL, so you can interact with it directly.

### Connect from a Client

In a second terminal, start a regular Rayforce REPL and open a connection:

```text
;; Open a handle to the server
(set h (.ipc.open "127.0.0.1:5000"))
```

The `.ipc.open` function returns a connection handle. Use `.ipc.send` to send a query string to the server, which parses, evaluates, and returns the result:

```text
;; Send a query, get the result back
(.ipc.send h "(+ 1 2)")
;; => 3
```

When you are done, close the connection:

```text
(.ipc.close h)
```

## 2. Loading Data on the Server

A server is most useful when it holds data that clients query remotely. Use an init script to load data at startup.

### Start with an Init Script

```bash
./rayforce -p 5000 init.rfl
```

The server executes `init.rfl` before accepting connections. A typical init script creates tables:

```lisp
;; init.rfl — load data into the server
(set trades (table [sym price qty time]
  (list [AAPL GOOG AAPL MSFT GOOG]
        [150.5 2800.0 151.2 310.0 2795.0]
        [100 50 200 75 120]
        [09:30:00 09:30:01 09:30:05 09:30:10 09:30:12])))

(set quotes (table [sym bid ask]
  (list [AAPL GOOG MSFT]
        [150.0 2790.0 309.5]
        [150.5 2800.0 310.5])))

(println "Loaded trades and quotes")
```

### Query from a Client

Once the server has data, clients can query it remotely:

```text
(set h (.ipc.open "127.0.0.1:5000"))

;; Query the remote trades table
(.ipc.send h "trades")
;; => the full trades table is returned

;; Run a filtered query
(.ipc.send h "(select {from: trades where: (== sym 'AAPL)})")

(.ipc.close h)
```

## 3. Remote Queries

The server evaluates any valid Rayfall expression sent as a string. This means you can run filters, aggregations, joins, and any other operation remotely.

### Filtered Queries

```text
;; All trades where price exceeds 100
(.ipc.send h "(select {from: trades where: (> price 100)})")
```

### Aggregations

```text
;; Total quantity traded
(.ipc.send h "(select {from: trades total: (sum qty)})")

;; Volume-weighted average price by symbol
(.ipc.send h "(select {from: trades by: sym vwap: (/ (sum (* price qty)) (sum qty))})")
```

### Joins

```text
;; Join trades with quotes on sym
(.ipc.send h "(select {from: (left-join [sym] trades quotes)})")
```

!!! note "Tip"

    The query string is parsed and evaluated entirely on the server. Only the final result is serialized and sent back to the client. This means large intermediate results never cross the network.

## 4. Dynamic Queries & Expression Payloads

`.ipc.send` accepts any serializable value, not just strings. The server behavior depends on what you send:

| Payload type | Server behavior |
|---|---|
| String | Parsed as Rayfall code and evaluated |
| List with function head | Evaluated as a function call — no parsing needed |
| Data (vector, table, atom) | Evaluated as identity — returned as-is |

### Expression payloads

Instead of strings, construct the query as a list. Builtins resolve to function objects — use them directly as the list head. Dict literals are self-evaluating, so column references inside them are preserved for server-side resolution:

```text
;; Arithmetic
(.ipc.send h (list + 1 2))
;; => 3

;; Select with filter — dict is self-evaluating, select resolves columns
(.ipc.send h (list select {from: trades where: (> price 200)}))
;; => filtered trades table

;; Aggregation by group
(.ipc.send h (list select {from: trades by: sym total: (sum qty)}))

;; Map a lambda over server-side data
(.ipc.send h (list map (fn [x] (* x 2)) (list til 10)))
;; => [0 2 4 6 8 10 12 14 16 18]
```

For dynamic queries, substitute runtime values into the expression:

```text
;; Dynamic filter threshold
(set threshold 200)
(.ipc.send h (list select {from: trades where: (list > (quote price) threshold)}))

;; Dynamic grouping
(set group-col 'sym)
(.ipc.send h (list select {from: trades by: group-col total: (list sum (quote qty))}))
```

!!! note "How it works"

    Dict literals are self-evaluating — `{from: trades where: (> price 200)}` preserves its contents as data. When the server evaluates the list, `select` resolves `trades` and `price` in the table context. Use `(list ...)` inside the dict when you need to splice runtime values into an expression.

    Because the query is built as data, `(quote price)` and `(quote qty)` evaluate to the literal symbols `'price` and `'qty` as each `(list ...)` is constructed. A literal symbol that names a from-table column resolves to that column during query evaluation, so they resolve to the `price` and `qty` columns. The tick forms `'price`/`'qty`, and bare `price`/`qty`, work the same way.

## 5. Async Fire-and-Forget

`.ipc.send` blocks until the server replies. When you don't need a reply — firing a write, pushing an update, kicking off background work — use `.ipc.post` instead. It sends the message and returns immediately.

```text
(set h (.ipc.open "127.0.0.1:5000"))

;; Fire-and-forget: returns the null object as soon as the send completes
(.ipc.post h "(set last-update 42)")

(.ipc.close h)
```

On the server, async messages are handled by the `.ipc.on.async` hook (or, with no hook installed, evaluated like any other message). No result is sent back, so a server-side error is logged on the server and never reaches the sender. `.ipc.post` only reports **local** failures: a `type` error if the handle isn't an integer or the message isn't serialisable, or an `io` error if the connection is closed.

Messages on a single connection are processed in send order, so a `.ipc.post` followed by a `.ipc.send` on the same handle will have been applied on the server by the time the synchronous reply comes back.

Use `.ipc.send` when you need the result or need to know the server succeeded; use `.ipc.post` when throughput matters and the send is one-way. See the [`.ipc.post` reference](../namespaces/ipc.md#ipc-post) for the full signature and error semantics.

## 6. Authentication

Rayforce supports password-based authentication with an optional read-only restriction.

### Password Authentication

Start the server with `-u` to require a password from all clients:

```bash
./rayforce -p 5000 -u secretpass
```

Clients must supply the password when connecting:

```text
(set h (.ipc.open "127.0.0.1:5000:user:secretpass"))
```

The connection string format is `host:port:user:password`. The username is transmitted but not validated on the server — only the password is checked.

### Read-Only Mode

Use `-U` instead of `-u` to enable authentication with read-only restrictions:

```bash
./rayforce -p 5000 -U secretpass
```

In read-only mode, clients can run queries and read data, but mutating operations (`set`, `insert`, `upsert`, `update`, file writes, system commands) are blocked. The server returns an error if a restricted builtin is called.

!!! note "Reference"

    See the [IPC & Serialization](../storage/ipc.md) reference page for the full list of restricted builtins in read-only mode.

## 7. Multi-Process Architecture

A common pattern is one data server with multiple query clients. Each client connects independently and runs queries against the shared data.

### Example: Trade Analytics System

**Terminal 1 — Data server:**

```bash
# Start the server with trade data
./rayforce -p 5000 init.rfl
# Server output: "Loaded trades and quotes"
# Server is now listening on port 5000
```

**Terminal 2 — Analytics client:**

```text
./rayforce

;; Connect to the data server
(set h (.ipc.open "127.0.0.1:5000"))

;; Run aggregations
(.ipc.send h "(select {from: trades by: sym avg_price: (avg price) total_qty: (sum qty)})")

;; Find the most active symbol
(.ipc.send h "(select {from: trades by: sym n: (count) desc: 'n take: 1})")
```

**Terminal 3 — Monitoring client:**

```text
./rayforce

(set h (.ipc.open "127.0.0.1:5000"))

;; Check current row count
(.ipc.send h "(count trades)")
;; => 5

;; Check table schema
(.ipc.send h "(meta trades)")
```

!!! note "Note"

    The server is single-threaded. Client queries are processed sequentially in the order they arrive. For CPU-intensive workloads, consider partitioning data across multiple server processes.

## 8. Error Handling

Several categories of errors can occur during IPC operations.

### Connection Refused

If the server is not running or the port is wrong, `.ipc.open` returns an error:

```text
(.ipc.open "127.0.0.1:9999")
;; => error: connection refused
```

### Authentication Errors

Connecting without credentials to a password-protected server, or supplying the wrong password:

```text
;; No credentials — server requires auth
(.ipc.open "127.0.0.1:5000")
;; => error: authentication required

;; Wrong password
(.ipc.open "127.0.0.1:5000:user:wrongpass")
;; => error: authentication failed
```

### Read-Only Restriction

When connected to a `-U` server, mutating operations are rejected:

```text
;; Trying to set a variable on a read-only server
(.ipc.send h "(set x 42)")
;; => error: restricted in read-only mode
```

### Network Errors

If the server shuts down or the network drops while a query is in flight, `.ipc.send` returns an error. Use `try` to handle errors gracefully:

```text
(try
  (println "Trade count:" (.ipc.send h "(count trades)"))
  (fn [e] (println "Query failed:" e)))
```

## 9. C API

The IPC layer is also accessible from C, enabling you to embed Rayforce clients in applications.

### Functions

| Function | Description |
|---|---|
| `ray_ipc_connect(host, port, user, password, timeout_ms)` | Open a TCP connection. Returns a handle. Pass `NULL` for user/password if no auth, and `0` for `timeout_ms` to use the default connect timeout. |
| `ray_ipc_send(handle, msg)` | Synchronous send: serialize `msg`, send, wait for response, return deserialized result. |
| `ray_ipc_send_async(handle, msg)` | Fire-and-forget send: serialize and send `msg` without waiting for a response. |
| `ray_ipc_close(handle)` | Close the connection and free the handle. |
| `ray_poll_set_restricted(poll, restricted)` | Make connections created afterward evaluate in restricted (read-only) mode. Call before servicing an embedded listener. |
| `ray_ipc_listen(poll, port)` | Register a server socket on the given poll loop. Used internally by `-p` mode. |

An embedded read-only server can use only the public, pointer-based API:

```c
ray_poll_t* poll = ray_poll_create();
ray_poll_set_restricted(poll, true);
ray_runtime_set_poll(poll);

ray_t* listener = ray_eval_str("(.sys.listen 7701)");
/* Check listener for an error, then service IPC from the host loop. */
ray_poll_run_for(poll, 200);
ray_release(listener);
```

The restriction applies to connections created after the setter call;
existing connections keep their original evaluation mode.

### Example: C Client

```c
#include <rayforce.h>
#include "core/ipc.h"

int main(int argc, char** argv) {
    ray_runtime_t* rt = ray_runtime_create(argc, argv);

    // Connect to a running server (returns handle or negative error)
    int64_t h = ray_ipc_connect("127.0.0.1", 5000, NULL, NULL, 0);
    if (h < 0) { printf("Connection failed\n"); return 1; }

    // Send a query string
    ray_t* query = ray_str("(+ 1 2)", 7);
    ray_t* result = ray_ipc_send(h, query);
    ray_release(query);

    if (!RAY_IS_ERR(result)) {
        printf("Result: %lld\n", result->i64);
        ray_release(result);
    }

    ray_ipc_close(h);
    ray_runtime_destroy(rt);
    return 0;
}
```

!!! note "Async sends"

    Async sends are useful for logging or fire-and-forget inserts where you do not need the server's response. Use `ray_ipc_send_async` to avoid blocking on the round-trip.

## Next Steps

- [**IPC & Serialization Reference**](../storage/ipc.md) — Wire format specification and full API details
- [**Storage Guide**](storage.md) — Persisting data with CSV, columnar files, and partitioned tables
- [**Core C API**](../c-api/core.md) — Working with `ray_t` objects, vectors, and tables in C
- [**Getting Started Tutorial**](../getting-started/tutorial.md) — Hands-on introduction to Rayfall
