# Load parted table `get-parted`

Accepts two arguments: string path to a parted tables root and a symbol name of a table. 

```clj
(set t (get-parted "/tmp/db/" 'tab))
```