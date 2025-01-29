# Read CSV file `read-csv`

Reads a CSV file into a table.
Accepts two arguments: vector of symbols representing column types and a string path to a CSV file.

```clj
(set t (read-csv [I64 I64 Symbol Timestamp] "/tmp/data.csv"))
```