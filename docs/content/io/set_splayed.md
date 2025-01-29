# Set splayed `set-splayed`

Store a table as a splayed table into a disk.

```clj
(set-splayed "/tmp/db/tab/" t)
```

Where `t` is a table.

Optionally accepts a string path to a symfile of the table.

```clj
(set-splayed "/tmp/db/tab/" t "/tmp/db/sym")
```
