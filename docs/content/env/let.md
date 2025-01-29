# Let `let`

`let` is a special form that allows you to define local variables. It is used to create a new variable and assign a value to it. The variable is only available within the scope of the `let` form.

```clj
↪ (do (let a 123) (+ a 1))
124
> a
•• [E003] error: object evaluation failed
╭─[0]─┬ repl:1..1 in function: @anonymous
│ 1   │ a
│     ┴ ‾ undefined symbol: 'a
```

In the example above, the variable `a` is defined within the `let` form and is not available outside of it.

`let` can also be used to define local variables inside a function body.

```clj
↪ (set add (fn [a b] (let c (+ a b)) c))
↪ (add 1 2)
3
```