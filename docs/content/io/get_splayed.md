# Load splayed table `get-splayed`

Accepts two arguments: string path to a splayed table and a string path to a symfile of the table. 

```clj
(set t (get-splayed "/tmp/db/tab/" "/tmp/db/tab.sym"))
```

Second argument is optional. If provided, it will be used as the symfile. Otherwise, the symfile will be inferred from the path to the table.

```clj
(set t (get-splayed "/tmp/db/tab/"))
```

!!! tip
    Pay attention to the trailing slash in the path to the table. It is required.
