# Load splayed table `get-splayed`

Accepts two arguments: string path to a splayed table and a string path to a symfile of the table. 

```clj
(set t (get-splayed "/tmp/db/tab" "/tmp/db/tab.sym"))
```