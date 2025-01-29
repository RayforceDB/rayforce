# Raise `raise`

The `raise` statement allows you to raise an exception. Accepts string as an argument.

```clj
↪ (raise "My exception")
•• [E017] error: raised error
╭─[0]─┬ repl:1..1 in function: @anonymous
│ 1   │ (raise "My exception")
│     ┴        ‾‾‾‾‾‾‾‾‾‾‾‾‾‾ My exception
```