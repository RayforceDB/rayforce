# Names and scope

Rayfall reads a name from the nearest binding visible at that expression. A
parameter or `let` binding hides a global of the same name. Inside a query, a
source column also hides a global; a parameter or `let` binding can in turn hide
that column.

## Global and local bindings

`set` binds a global name. `let` binds in the current frame: the top level, a
function call, or a `do` block. Function parameters are local to that call.

```lisp
(set x 10)
(do (let x 20) x)                 ; 20
x                                 ; 10
((fn [x] x) 30)                  ; 30
```

`do` creates a new frame. `while` does not: a `let` in its body belongs to the
enclosing frame. A later `let` of the same name in that frame changes the
value subsequent reads see.

## Names in queries

A source column takes precedence over a global of the same name. To use a
global, bind it to a different local name before the query, or pass it as a
parameter to a helper. A local parameter or `let` named like a column hides
that column.

```lisp
(set price 100)
(set trades (table [price] (list [10 20])))
(select {p: price from: trades})  ; p is [10 20], from the column
```

An output alias does not create a binding for another expression in the same
query. A quoted symbol used as data can also name a column in query context;
see [Select](../queries/select.md) for the query-specific rules.

## Closures

A closure captures the local bindings visible where its `fn` is written. It
does not capture locals of whoever calls it. A name that was not captured is
resolved when the closure runs, so a later global `set` can change its value.

```lisp
(set g 1)
(set make (fn [x] (fn [] (+ x g))))
(set f (make 10))
(set g 2)
(f)                               ; 12: captured x, current global g
```

`eval` and `resolve` inside a function body can see that body's locals and
captures. `(resolve 'x)` reads the currently visible binding of `x`; `eval`
evaluates a parsed expression in the current context.

## Mutating a named table

The quoted-symbol forms `(insert 't rows)`, `(upsert 't keys rows)` and
`(alter 't ...)` rebind the *global* `t` by name. They do not mutate a local
parameter or `let` binding. In particular, `alter` rejects a local target
with a `domain` error. To work with a local table, use the value-returning
form and bind its result explicitly.

Names beginning with `.` are reserved for system namespaces such as `.sys`
and `.db`. `set`, `let`, parameters, and `del` cannot bind or delete them.
This rule is the same in interpreted expressions and compiled function
bodies; compiled closures capture locals at creation rather than reading a
caller's frame.
