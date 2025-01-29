# Enum `enum`

Enumerates one symbol vector over another.

```clj
↪ (set a [a b c])
[a b c]
↪ (enum 'a [a b c a c c c c a])
'a#[a b c a c c c c a]
```

Useful for saving space when storing repeated symbols and for splayed tables with symbolic columns.