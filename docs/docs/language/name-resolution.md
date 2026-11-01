# Names and scope

Rayfall reads a name from the nearest binding visible at that expression. A
parameter or `let` binding hides a global of the same name. Inside a query, a
source column also hides a global; a parameter or `let` binding can in turn hide
that column.

## Global and local bindings

`set` binds a global name. `let` binds in the current frame: the top level, a
function call, or an interpreted `do` block. Function parameters are local to
that call.

```lisp
(set x 10)
(do (let x 20) x)                 ; 20
x                                 ; 10
((fn [x] x) 30)                  ; 30
```

At the top level, interpreted `do` pushes a frame, as the example shows. A
compiled function body runs `do` inline: a `let` inside it can rebind a
parameter and remain visible after the `do`:

```lisp
((fn [x] (do (let x 20)) x) 10) ; 20
```

`while` does not push a frame on either route: a `let` in its body belongs to
the enclosing frame. A later `let` of the same name in that frame changes the
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

A later projection can read an earlier output alias in the same `select`:

```lisp
(select {p: price q: (+ p 1) from: trades}) ; q is [11 21]
```

The alias is bound after its own expression. `where:` and `by:` use the source
table rather than projection aliases; sort keys have their own scope rules.
A quoted symbol used as data can also name a column in query context. See
[Select](../queries/select.md) for these query-specific rules.

## Closures

A closure captures the local bindings visible where its `fn` is written. A
caller's local cannot replace a *captured* binding. An uncaptured name is
resolved when the closure runs: it can see a later global `set`, or an ordinary
local in an interpreted caller's frame.

```lisp
(set g 1)
(set make (fn [x] (fn [] (+ x g))))
(set f (make 10))
(set g 2)
(f)                               ; 12: captured x, current global g
```

For an uncaptured function name, the caller's interpreted local can win:

```lisp
(set namedfn (fn [v] (+ v 1)))
(set closedfn (fn [v] (namedfn v)))
(do (let namedfn (fn [v] 99)) (closedfn 3)) ; 99
```

`eval` and `resolve` inside a function body can see that body's locals and
captures. `(resolve 'x)` reads the currently visible binding of `x`; `eval`
evaluates a parsed expression in the current context.

## Mutating a named table

The quoted-symbol forms of `insert` and `alter` resolve the name to the
innermost visible local or parameter, then to a global, and publish their
result back to that same binding. They amend a sole-owned local in place; a
shared value (an alias, a capture, a slice) is copied first, so its other
holders do not see the write. An unbound target is an error, and an error
never redirects a local write into a same-named global:

```lisp
(do (let local_t (table [x] (list (til 2))))
    (insert 'local_t (list 3))
    (count local_t))                 ; 3, amended in place
```

This holds when the call's head is the literal `insert` or `alter`, or a
global alias of one, such as `(set ins insert)`, bound when the function is
compiled. A mutator reached through any other head (a parameter, a `let`, a
dotted head, a computed head, or a name unbound at that time) is an ordinary
callee: a quoted target resolves as a global and a computed target publishes
nothing, whether the function is compiled or left to the tree walker. See [`insert` / `upsert`](syntax.md#insert-upsert).

`upsert` is unchanged: its quoted form still writes back to a global name
when it rebuilds the result, so for a local table use the value-returning
form and bind its result explicitly.

Names beginning with `.` are reserved for system namespaces such as `.sys`
and `.db`; ordinary dotted names cannot be rebound, and parameters cannot
use reserved names. The five connection-hook slots `.ipc.on.open`,
`.ipc.on.close`, `.ipc.on.sync`, `.ipc.on.async`, and `.ipc.on.auth` are the
exception: `set` installs a hook and `del` clears it. See
[IPC Connection Hooks](../storage/ipc-hooks.md).
