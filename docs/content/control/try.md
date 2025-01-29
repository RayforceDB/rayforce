# Try `try`

`try` is a control structure that allows you to handle exceptions in a more controlled way. It accepts two arguments: the expression to be evaluated and an expression to be evaluated in case of an exception.

```clj
↪ (try (raise "A") 1)
1
>(try (raise "A") (fn [x] x))
"A"
```