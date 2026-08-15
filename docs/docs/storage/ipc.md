# IPC & Serialization

TCP client-server IPC with binary serialization, delta compression, and sync/async messaging.

!!! note "New to IPC?"
    Start with the [IPC Guide](../guides/ipc.md) for a hands-on walkthrough of server setup, remote queries, authentication, and multi-process patterns. This page is a technical reference.

## Overview

Rayforce provides a complete IPC system for client-server communication. A server listens on a TCP port, accepts connections, and evaluates queries sent by clients. The wire format uses the same compact binary serialization for all Rayforce types, with automatic delta/RLE compression for large payloads.

## Server Mode

Start Rayforce as an IPC server with the `-p` flag:

```bash
# Start server on port 5000
./rayforce -p 5000

# Run a script first, then serve
./rayforce -p 5000 init.rfl

# Server with authentication required
./rayforce -p 5000 -u mypassword

# Server with authentication + read-only restriction
./rayforce -p 5000 -U mypassword
```

The server runs in the same thread as the REPL. IPC connections are processed between REPL inputs. When piped input is exhausted, the server continues running until interrupted.

## Handshake

Every IPC connection opens with a 2-byte exchange before any framed payload flows: `{ wire_version, auth_flag }`. The client sends its `RAY_SERDE_WIRE_VERSION` and a zero; the server replies with its own wire version and `0x01` if password auth is required, `0x00` otherwise. Either side will drop the connection if the peer's first byte doesn't match — this is what prevents a new peer from ever writing a newer-format payload to a peer that couldn't parse it.

## Authentication

The `-u` and `-U` flags enable password authentication for IPC connections. Clients must provide valid credentials during the handshake or the connection is rejected.

### -u (password required)

```bash
# Server: require password
./rayforce -p 5000 -u secret123
```

```text
; Client: connect with credentials
(set h (.ipc.open "127.0.0.1:5000:admin:secret123"))
(.ipc.send h "(+ 1 2)")
;; => 3
```

All clients must authenticate. The username field is transmitted but not validated — only the password is checked.

### -U (password + restricted mode)

```bash
# Server: require password, restrict IPC to read-only
./rayforce -p 5000 -U secret123
```

In restricted mode, specific builtins that write files, mutate state, or control the server process are blocked for IPC connections. Queries, aggregations, and read-only operations are allowed. The blocked set is the full `RAY_FN_RESTRICTED` builtin list; it includes (but is not limited to):

| Category | Blocked Builtins |
|---|---|
| Mutation | `set`, `del`, `update`, `insert`, `upsert`, `modify` |
| File writes | `write`, `write-bytes`, `.csv.write`, `load`, `.db.splayed.set` |
| File reads | `read`, `read-bytes`, `.csv.read` |
| System | `.sys.exec`, `.os.getenv`, `.os.setenv`, `exit` |
| IPC chaining | `.ipc.open`, `.ipc.close`, `.ipc.send`, `.ipc.post` |

!!! note "Scope"
    Restricted mode blocks every builtin registered with `RAY_FN_RESTRICTED` (the table above is a representative subset). It does not provide full sandboxing — `select`, `.db.splayed.get`, `.db.parted.get`, and other read operations on already-loaded data or splayed tables remain available.

Attempting a restricted operation returns an `"access"` error:

```text
;; On a -U server:
(.ipc.send h "(set x 42)")
;; => error: access — restricted

(.ipc.send h "(+ 1 2)")
;; => 3  (queries still work)
```

!!! note "Security note"
    Restriction applies to all dispatch paths including higher-order functions. `(map .sys.exec ["echo hi"])` is also blocked in restricted mode.

## Client Builtins

Connect to a running server and send queries from Rayfall:

### .ipc.open

```text
;; Connect to a server (no auth)
(set h (.ipc.open "127.0.0.1:5000"))
;; => 0  (connection handle)

;; Connect with credentials (when server uses -u or -U)
(set h (.ipc.open "127.0.0.1:5000:admin:secret123"))
;; => 0  (connection handle)
```

The format is `"host:port"` for unauthenticated connections, or `"host:port:user:password"` when the server requires authentication. If the server requires auth and no credentials are provided, `.ipc.open` returns an `"access"` error.

### .ipc.send

`.ipc.send` sends any serializable value to the server and returns the result. The server's behavior depends on the payload type:

| Payload type | Server behavior |
|---|---|
| String | Parsed as Rayfall code and evaluated; result returned |
| Any other value | Evaluated directly (identity for data, execution for expressions); result returned |

#### String queries

```text
;; Arithmetic — server parses and evaluates the string
(.ipc.send h "(+ 1 2)")
;; => 3

;; Look up a server-side variable
(.ipc.send h "trades")
;; => <table>

;; Remote select with filter, sort, limit
(.ipc.send h "(select {from: trades where: (> price 100) desc: 'price take: 10})")
;; => top 10 most expensive trades

;; Aggregation by group
(.ipc.send h "(select {from: trades by: sym total: (sum qty) avg_px: (avg price)})")
;; => per-symbol totals
```

#### Expression payloads

Non-string values are evaluated directly on the server via `ray_eval`. Construct executable expressions as lists with builtin function objects as heads. Dict literals are self-evaluating, so column references and expressions inside them are preserved unevaluated until the server processes them:

```text
;; Arithmetic
(.ipc.send h (list + 1 2))
;; => 3

;; Select with filter — dict stays unevaluated, select resolves columns
(.ipc.send h (list select {from: trades where: (> price 200)}))
;; => filtered trades table

;; Aggregation by group
(.ipc.send h (list select {from: trades by: sym total: (sum qty)}))

;; Map a lambda over server-side data
(.ipc.send h (list map (fn [x] (* x 2)) (list til 10)))
;; => [0 2 4 6 8 10 12 14 16 18]
```

For dynamic queries, substitute values into the dict at construction time:

```text
(set threshold 200)
(.ipc.send h (list select {from: trades where: (list > (quote price) threshold)}))
;; => trades where price > 200
```

The query is assembled as data: while the `(list ...)` is built, `(quote price)` evaluates to the literal symbol `'price`, so the predicate becomes `(> 'price threshold)`. A literal symbol that names a from-table column resolves to that column during query evaluation, so `'price` resolves to the `price` column. Writing the tick form `'price`, or a bare `price`, works the same way.

### .ipc.close

```text
;; Close the connection
(.ipc.close h)
```

!!! note "Message types"
    `.ipc.send` uses synchronous messaging — it blocks until the server returns a result. For asynchronous (fire-and-forget) messaging, use `.ipc.post`, which sends without waiting for a response (the C equivalent is `ray_ipc_send_async`).

## Connection Hooks

The server side exposes the inbound connection lifecycle to Rayfall code through five user-installable lambdas under `.ipc.on.*` — `open`, `close`, `sync`, `async`, and `auth` — plus the `(.ipc.handle)` accessor that returns the current connection's handle inside any hook. See [IPC Connection Hooks](ipc-hooks.md) for the full reference: signatures, install / clear semantics, the reserved-namespace carve-out, per-hook error handling, and the restricted-mode interaction.

## Serialization with `ser`

The `ser` builtin converts any value to a binary buffer (a `U8` vector). Pass it any Rayforce value — atom, vector, list, or table:

```lisp
;; Serialize an integer
(ser 42)
;; => [0xfa 0xde 0xfa 0xce 0x03 0x00 0x00 0x00 ..]

;; Serialize a vector
(ser (til 10))

;; Serialize a string
(ser "hello")

;; Serialize a table
(set t (table [x y] (list [1 2 3] ['A 'B 'C])))
(ser t)
```

The result is always a `U8` byte vector containing the IPC header followed by the serialized payload.

## Deserialization with `de`

The `de` builtin reconstructs a value from its binary representation. Compose it with `ser` for a perfect round-trip:

```lisp
;; Round-trip an integer
(de (ser 42))
;; => 42

;; Round-trip a vector
(de (ser (til 10)))
;; => [0 1 2 3 4 5 6 7 8 9]

;; Round-trip a string
(de (ser "hello"))
;; => "hello"

;; Round-trip a float
(de (ser 3.14))
;; => 3.14

;; Round-trip a boolean
(de (ser true))
;; => true

;; Round-trip a list of mixed types
(de (ser (list 1 "two" 3.0)))
;; => (1 "two" 3.0)

;; Round-trip a table
(set t (table [x y] (list [1 2 3] ['A 'B 'C])))
(de (ser t))
;; => the same 3-row table with columns x (i64) and y (sym)
```

## Wire Format

Every serialized payload begins with a 16-byte `ray_ipc_header_t` header, followed by the serialized object bytes. The header layout:

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 4 bytes | `prefix` | Magic bytes `0xcefadefa` — identifies Rayforce binary data |
| 4 | 1 byte | `version` | Wire-format version (`RAY_SERDE_WIRE_VERSION`, currently `3`). Decoupled from `RAY_VERSION_MAJOR`. A receiver that sees a different byte here rejects the payload with a `version` error instead of attempting to parse it. |
| 5 | 1 byte | `flags` | Bit 0 (`0x01`): compressed (0 = no, 1 = yes). Bit 1 (`0x02`, `RAY_IPC_FLAG_RESTRICTED`): journal-only — records that a persisted frame arrived on a `-U` restricted connection so replay re-imposes the restriction; ignored on the live wire. Bit 2 (`0x04`, `RAY_IPC_FLAG_VERBOSE`): request that the server capture the eval's stdout/stderr and return `[captured_str, result]` (used by the remote-REPL wrapper). |
| 6 | 1 byte | `endian` | Endianness: `0` = little-endian |
| 7 | 1 byte | `msgtype` | Message type: `0` = async, `1` = sync, `2` = response |
| 8 | 8 bytes | `size` | Payload size in bytes (`int64`) |

The corresponding C struct:

```c
typedef struct ray_ipc_header_t {
    uint32_t prefix;     /* RAY_SERDE_PREFIX (0xcefadefa) */
    uint8_t  version;    /* RAY_SERDE_WIRE_VERSION (currently 3) */
    uint8_t  flags;      /* bit0 compressed, bit1 restricted (journal), bit2 verbose */
    uint8_t  endian;     /* 0 = little-endian */
    uint8_t  msgtype;    /* 0 = async, 1 = sync, 2 = response */
    int64_t  size;       /* payload size in bytes */
} ray_ipc_header_t;
```

The header is exactly 16 bytes, enforced by a compile-time static assertion.

### Wire-version history

- **v2** — atoms serialized as `type(1) + value-bytes`. Typed-null atoms (`0Nl`, `0Nf`, …) lost their null bit on round-trip.
- **v3** (current) — atoms serialized as `type(1) + flags(1) + value-bytes`. Bit 0 of `flags` carries the typed-null marker; `(de (ser 0Nl))` now returns `0Nl`. The [handshake byte](#handshake) advertises the same version so peers speaking different wire versions are closed before any payload is exchanged.

Because the version field is checked symmetrically on send and receive, a v3 peer will refuse to connect to a v2 peer (and vice versa) rather than silently mis-parsing.

## Compression

Payloads larger than 2,000 bytes are automatically compressed using delta + RLE encoding. This works especially well for sorted columnar data (long runs of identical delta bytes).

- **Delta encoding** — each byte is replaced by the difference from the previous byte
- **RLE** — runs of identical bytes are stored as (count, value) pairs
- **Threshold** — payloads ≤ 2,000 bytes are sent uncompressed
- **Transparent** — compression is automatic; the `flags` header byte signals whether the payload is compressed

If compression doesn't reduce the payload size, the data is sent uncompressed.

## Supported Types

All core Rayforce types serialize and deserialize faithfully:

| Category | Types |
|---|---|
| Integer atoms | `i64`, `bool` |
| Float atoms | `f64` |
| String atoms | `str`, `sym` |
| Temporal atoms | `date`, `time`, `timestamp` |
| Other atoms | `guid`, `null` |
| Vectors | All typed vectors (`I64`, `F64`, `BOOL`, `STR`, `SYM`, `DATE`, `TIME`, `TS`, `GUID`), including sentinel null state |
| Collections | `list` (heterogeneous, nested), `dict` |
| Tables | `table` (column names + column vectors) |

## C API

The serialization functions are declared in `src/store/serde.h`:

| Function | Description |
|---|---|
| `ray_ser(obj)` | Serialize `obj` to a `U8` vector with IPC header |
| `ray_de(bytes)` | Deserialize from a `U8` vector, validates IPC header |
| `ray_serde_size(obj)` | Calculate serialized size (excluding header) |
| `ray_ser_raw(buf, obj)` | Serialize into a caller-provided buffer (no header) |
| `ray_de_raw(buf, len)` | Deserialize from raw buffer, updates `len` with bytes consumed |

## Use Cases

- **Remote queries** — run analytics on a server-side dataset from a client REPL
- **Multi-process architecture** — one server process owns the data, multiple clients query it
- **In-memory data exchange** — `ser` / `de` round-trip any Rayforce value through a compact byte vector
- **Table persistence** — for saving tables to disk, use splayed/partitioned storage (see [Storage Guide](../guides/storage.md))

## Limitations

- **No streaming** — the entire object is serialized at once; there is no chunked or incremental mode
- **Single-threaded server** — the server processes queries sequentially on the main thread. Long-running queries block other clients.
- **Plaintext transport** — data travels unencrypted over TCP. Use on trusted networks or tunnel through SSH for security.
- **Shared secret only** — authentication uses a single password for all clients (`-u`/`-U`). There is no per-user access control.
