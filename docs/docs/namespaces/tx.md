# `.tx.*` — transactions

A transaction groups changes so that all of them happen or none does. It is opened and closed by the connection that wants it and lasts as many messages as that takes:

```lisp
(.tx.begin)
(insert 'trades (list 1 100.0 'AAPL))
(update {from: 'positions qty: (+ qty 1) where: (== sym 'AAPL)})
(.db.splayed.set "/data/positions/" positions)
(.tx.commit)        ;; or (.tx.rollback)
```

!!! note "Restricted under `-U`"
    All three verbs are `RAY_FN_RESTRICTED`.

## Reference

| Function | Arity | Description |
|---|---|---|
| `.tx.begin` | variadic | Open a transaction owned by the caller. `(.tx.begin idle-ms total-ms)` sets its time limits. |
| `.tx.commit` | nullary | Make the transaction's changes durable and visible. |
| `.tx.rollback` | nullary | Undo everything since `.tx.begin`. |

Errors: `tx` (`.tx.begin` while one is open; `.tx.commit` or `.tx.rollback` with none open, or with one that was rolled back), `arity` / `type` (bad limits), `io` (the commit could not be logged; the transaction is rolled back).

## What it guarantees

**Atomic.** `.tx.rollback` puts every global back to what it held at `.tx.begin`: names created are gone, names deleted are back, tables written to disk are as they were. An error in any message of the transaction rolls all of it back by itself.

**Isolated.** There is one transaction at a time, and while it is open **the server serves nobody but its owner**. Other connections — readers too — timers and the local console wait for it to end, then see all of it or none. The owner reads its own writes, in memory and on disk.

**Durable.** With a journal open ([`-L`](log.md), or `.log.open 'sync`), the changes are held back from the log and written as one commit by `.tx.commit`; the reply to `.tx.commit` is sent after that commit is on disk. A process killed before then recovers to the state before `.tx.begin`. Under `-l` the commit is handed to the operating system without `fsync`. Without a journal there is nothing to recover from.

**Consistent** in the sense the other three give: nobody observes half of a transaction.

This is the model of SQLite's `BEGIN EXCLUSIVE`, not of a multi-version engine: there are no concurrent transactions to conflict, and no reader runs beside a writer.

## After an error

When a message inside a transaction fails, the transaction is rolled back and that message's reply is its error. The owner's connection then answers every message with `tx` — and nothing it sends takes effect — until it sends `(.tx.rollback)`. Without this, the rest of a batch written for the transaction would run outside it, each change committed on its own.

```lisp
(.tx.begin)
(set n 2)
(no_such_function 1)    ;; error: name   — the transaction is rolled back
(set n 3)               ;; error: tx     — no effect
(.tx.rollback)          ;; acknowledges; the connection is back to normal
```

An error caught by `try` inside a message is not an error of the message.

## Time limits

A transaction stops the server for everyone else, so one opened over a connection is bounded:

| Limit | Default | Measures |
|---|---|---|
| idle | 5 s | the owner's silence between two of its messages |
| total | 60 s | the whole transaction |

`(.tx.begin idle-ms total-ms)` sets both; `0` lifts one. When a limit runs out the transaction is rolled back and the owner's connection is closed. A connection that closes for any reason has its transaction rolled back.

## From the REPL and from scripts

`.tx.begin` outside a connection opens a transaction owned by the local console, under the same rules but **without time limits**: whoever is at the console answers for the wait.

- **Terminal or piped REPL:** connections and timers wait until the transaction ends. Input that ends inside a transaction rolls it back.
- **Script (`rayforce file.rfl`):** the script runs before anything else is served. A script that fails or ends inside a transaction rolls it back and exits non-zero.
- A timer callback or connection hook that returns with a transaction open has it rolled back.

## Tables on disk

`.db.splayed.set` inside a transaction writes the new table in full and stops short of publishing it. Other processes keep reading the table as it was; the owner reads what it wrote. `.tx.commit` publishes every table the transaction wrote, `.tx.rollback` removes what was written.

- If the process dies after the commit reached the journal but before a table was published, recovery publishes it.
- A transaction never waits for a table's write lock. A table being written by another process, or written a second time in the same transaction, fails the write with `io` — and so rolls the transaction back.
- Without a journal the commit still publishes every table, but a crash in the middle can leave some published and some not.

## Not inside a transaction

A rollback could not take these back, so inside a transaction they fail with `access` (and, being an error, roll it back):

`.sys.exec`, `.sys.trust`, `.sys.listen`, `write`, `write-bytes`, `write-csv` / `.csv.write`, `.csv.splayed`, `.csv.parted`, `.parquet.splayed`, `.parquet.parted`, `hnsw-save`, `.db.parted.fill`, and `.log.open`, `.log.replay`, `.log.replay_report`, `.log.roll`, `.log.snapshot`, `.log.sync`, `.log.close`, `.log.purge`.

Not refused, and not undone either: timers set, environment variables set, connections opened and subscriptions made inside a transaction stay after a rollback.

## Costs

- **The first write to a table in a transaction copies it.** The transaction keeps the old table for the rollback, so the write cannot be made in place. Later writes to the same table in the same transaction are in place again.
- **Everyone waits.** Keep transactions short; a transaction that thinks between messages stops every other client for that long.
- A commit of an `update`, `delete`, `upsert` or `alter` logs the table it leaves; an `insert` logs its rows (see [the journal](log.md)).

## See also

- [`.log.*` — Write-ahead log](log.md) — where commits go.
- [`.db.*` — Tables on disk](db.md)
