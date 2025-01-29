# Format `format`

Accepts any Rayforce object and returns a string representation of it.

```clj
↪ (format 1)
"1"
↪ (format "% %" 1 3)
"1 3"
↪ (format "% %" 1 [1 2 3])
"1 [1 2 3]"
```