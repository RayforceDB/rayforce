# If `if`

Control flow function that evaluates a condition and executes a block of code if the condition is true.

```clj
↪ (if true (print "true") (print "false"))
true
↪ (if false (print "true") (print "false"))
false
↪ (if 1 3)
3
↪ (if 0 3)
```