# :material-remote: IPC

A Rayforce process can listen for IPC clients and evaluate Rayfall code
sent over the wire on their behalf.  This is the same channel the
remote REPL uses (see [Remote REPL](remote-repl.md)).

!!! warning
    The wire protocol is still evolving and may change between
    releases.  Server and client must be the same Rayforce version.

## :material-phone-log-outline: Listen on a port

Start a server with `-p`:

```bash
rayforce -p 5110
```

If `stdin` and `stdout` are both terminals, this also opens a local
REPL alongside the listener — handy for inspecting state while clients
connect.

When `stdin` and `stdout` are both **not** terminals (typical service
deployment under systemd, Docker, or `nohup &`), Rayforce auto-detects
service mode, skips the REPL, and just runs the IPC poll loop.  In
that case redirect `stdout` and `stderr` to a file or journal — any
`(println …)` printed by the server-side eval lands there:

```bash
nohup rayforce -p 5110 > rayforce.log 2>&1 &
```

```ini
# systemd unit
[Service]
Type=simple
ExecStart=/usr/bin/rayforce -p 5110
StandardOutput=journal
StandardError=journal
```

`-u PW` / `-U PW` set a connection password (the latter also marks the
session restricted, which blocks state-mutating builtins).

## :material-connection: Connect from another Rayforce

```clj
(set h (.ipc.open "127.0.0.1:5110"))
```

The address is `host:port` or `host:port:user:password` (the longer
form when the server requires auth).  `.ipc.open` returns an integer
handle on success or a Rayfall error on connect / auth / version
failure.

Send a request and wait for a response:

```clj
(.ipc.send h "(+ 1 2)")    ;; → 3
(.ipc.send h (list 1 2 3)) ;; → (1 2 3)
```

Two payload shapes are accepted:

* **String** — parsed and evaluated on the server (`ray_eval_str`).
  This is the natural shape for sending Rayfall source code.
* **Any other value** — evaluated as-is (`ray_eval`).  Useful when
  the client has already constructed an AST or just wants to ship
  data.

Always close the handle when you're done — server connection slots
are bounded:

```clj
(.ipc.close h)
```

## :material-fire: Fire-and-forget

If you don't need a response, send through a negated handle:

```clj
(.ipc.send (neg h) "(append-row! ...)")
```

The call returns immediately; the server processes the message but
sends no reply.  Use for telemetry pings, cache invalidations, etc.

## :material-database: Shared state — read this before deploying

**Every connected client and the local REPL share one global
environment.**  This is intentional (it matches kdb+/q's model and
makes ad-hoc collaborative analysis straightforward), but has
non-obvious consequences:

```clj
;; client A:
(.ipc.send h "(set x 100)")
;; client B (same server):
(.ipc.send h "x")            ;; → 100  ← B sees A's binding
;; server REPL (if running interactively):
‣ x                          ;; → 100  ← REPL sees it too
```

Because evaluation is single-threaded (one epoll loop drives both the
listener and the server-side REPL), individual `set`/`get` calls are
atomic from each client's point of view — no races on the binding
itself.  But **lost updates are possible**: if A and B both write to
`x` "at the same time", whichever the poll loop dequeued second wins.

### Recommended isolation: namespace per client

If you don't want clients to step on each other, give each client (or
each session) its own namespace and write everything under it:

```clj
;; client A
(.ipc.send h "(set .session-a.x 100)")
(.ipc.send h "(set .session-a.tab (table [...] (list ...)))")

;; client B works under .session-b.*
;; server-wide builtins still work for everyone
```

This is exactly how q-style `.user.*` conventions are used in
production deploys — Rayforce's symbol interner treats each
dotted-prefixed name as a separate slot in the global env, and
namespace boundaries are enforced by convention rather than runtime
checks.  Cheap and effective.

For full multi-tenant isolation (separate environments per
connection, no shared state at all) — open a feature request; that's
a different design and not currently supported.

## :material-message: Message types

Three over-the-wire variants:

| Type | Direction | Reply? |
|------|-----------|--------|
| `RAY_IPC_MSG_SYNC` | client → server | yes (server returns result) |
| `RAY_IPC_MSG_ASYNC` | client → server | no |
| `RAY_IPC_MSG_RESP` | server → client | implicit reply to a sync |

A `RAY_IPC_FLAG_VERBOSE` flag bit can be set on a SYNC request to ask
the server to capture whatever the eval prints to `stdout` / `stderr`
and return it alongside the result as a 2-element list
`[captured_str, result]`.  This is what the [Remote REPL](remote-repl.md)
uses to make `(println …)` from the connected client appear on the
client's terminal instead of the server's.  Most callers don't need
to set the flag manually — the helper builtins handle it.

## :material-handshake: Handshake & auth

When a client opens a connection, it sends a 2-byte handshake:
`[wire_version, padding]`.  The server replies with two bytes:
`[wire_version, auth_required]`.  If `auth_required` is set, the
client follows up with `length(1) | "user:password\\0"`; the server
responds with a 1-byte auth status.

Mismatched wire versions or wrong credentials drop the connection at
this point — `.ipc.open` surfaces those as `version` / `access`
errors.  Past handshake, all messages are `[16-byte header,
serialized payload]` framed.

## :material-protocol: Compression

Payloads larger than a fixed threshold are LZ4-compressed in-flight,
indicated by `RAY_IPC_FLAG_COMPRESSED`.  Decompression is automatic
on both sides; clients don't have to do anything.
