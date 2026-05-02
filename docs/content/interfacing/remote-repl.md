# :material-console-network: Remote REPL

A **remote REPL** is the local Rayforce REPL talking to a different
Rayforce instance over IPC.  You sit at your normal `‣` prompt, but
each line you type is evaluated on the connected server, with
results — including anything the server-side eval prints — rendered
locally on your terminal.

It builds entirely on top of [IPC](ipc.md): `.repl.connect` is a thin
wrapper over `.ipc.open`, and each request is a SYNC message with the
verbose-output flag set.  No new wire path, no separate event loop.

## :material-power-plug: Connect

Start a Rayforce server in one terminal (or anywhere reachable):

```bash
rayforce -p 5110
```

In your local Rayforce REPL:

```clj
‣ (.repl.connect "127.0.0.1:5110")
0
127.0.0.1:5110 ‣
```

The prompt prefix updates to the server address so you can never
mistake it for a local prompt.  `.repl.connect` returns the underlying
IPC handle (the same value `.ipc.open` would have returned); you don't
have to keep it — the REPL session knows it owns the connection.

The address can include credentials when the server is `-u`/`-U`-protected:

```clj
‣ (.repl.connect "127.0.0.1:5110:user:hunter2")
```

## :material-keyboard-return: Use it

Anything you type runs on the **server**:

```clj
127.0.0.1:5110 ‣ (+ 1 2)
3
127.0.0.1:5110 ‣ (set y 42)
42
127.0.0.1:5110 ‣ (* y 2)        ;; state persists across calls
84
127.0.0.1:5110 ‣ (println "hello from remote")
hello from remote               ;; printed locally, not on server
127.0.0.1:5110 ‣
```

Server-side `stdout` / `stderr` from your eval are captured per
request and streamed back to your terminal as part of the response —
*not* dumped on the server's console.  This is what makes the remote
REPL feel like a local one.

## :material-power-plug-off: Disconnect

Two ways:

```clj
127.0.0.1:5110 ‣ (.repl.disconnect)
‣
```

Or just hit `Ctrl-D` — the first `Ctrl-D` inside a remote session
disconnects (returns to the local prompt); a second `Ctrl-D` exits
Rayforce.  This mirrors `ssh` / `mosh` behaviour.

The explicit exit commands (`\`, `exit`, `:q`, `:quit`) always close
the whole local Rayforce regardless of session state.

## :material-account-multiple: One server, several clients

The server's evaluation environment is shared between **all**
connected clients and the server's own REPL.  Variables set by one
client are visible to everyone:

```clj
;; client A
127.0.0.1:5110 ‣ (set shared 100)
100

;; client B (different machine, same server)
127.0.0.1:5110 ‣ shared
100
```

This is a feature when you're collaborating on a shared dataset, and
a footgun when you aren't.  Rayforce evaluation is single-threaded so
there are no races on individual `set` calls — but lost-update is
possible: if two clients both write to `x`, whichever message the
server's poll loop dequeued second wins.

### Isolating yourself with a namespace

If you want a private workspace inside the shared server, prefix all
your bindings with a namespace:

```clj
127.0.0.1:5110 ‣ (set .my.x 100)
127.0.0.1:5110 ‣ (set .my.tab (table [a] (list (til 1000))))
```

`.my.*` slots are separate symbols from `x` / `tab`.  Other clients
can still inspect them if they want (the env is global), but they
won't accidentally clobber them.

## :material-cog: How it actually works

Under the hood:

* `(.repl.connect "host:port")` calls `.ipc.open` and stashes the
  handle plus the address in the local REPL state.  It also sets the
  prompt prefix on the local terminal.
* Each line of input you type is checked: if it starts with
  `(.repl.…` it runs **locally** (so you can always `.repl.disconnect`
  from inside a session).  Otherwise it's wrapped in a SYNC IPC
  request with `RAY_IPC_FLAG_VERBOSE` set and shipped to the server.
* The server captures `stdout`/`stderr` for the duration of the eval
  via `dup2` over a temp file, then returns
  `[captured_str, result]` as a 2-element list.
* The local REPL prints the captured bytes verbatim, then renders the
  result with the usual pretty-printer.
* `Ctrl-D` and `(.repl.disconnect)` both call `.ipc.close` and clear
  the prompt prefix.

There is no separate event loop, no extra wire format beyond the
existing IPC SYNC frame, and no special server-side mode — any
Rayforce server started with `-p` is automatically a valid remote-REPL
target.

## :material-alert-circle: Limitations

* **One remote session at a time per local REPL.**  Calling
  `.repl.connect` while already connected closes the previous session
  and opens the new one.  Multi-target tooling (q-style `h1 h2 h3`)
  is out of scope.
* **No interactive features over the wire.**  Tab completion, ghost
  text, syntax highlighting, multi-line editing — all run locally
  against the local symbol set, not the server's.  Long expressions
  span multiple lines exactly as they would in a local REPL; only the
  finalised text is shipped.
* **Async output isn't streamed.**  If the server-side eval prints
  in chunks during a long computation, you'll see all of it at once
  when the eval finishes (capture is start-to-end).  Real streaming
  is a future enhancement.
* **No reconnection.**  If the server goes away mid-session, the next
  request errors out and your session is effectively dead — you have
  to `.repl.connect` again.
