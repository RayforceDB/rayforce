# :material-code-braces: Environment Functions

RayforceDB provides functions for managing variables and the execution environment. These functions enable variable assignment, scoping, and environment introspection.

## Variable Assignment

### :material-pencil: Set

Assigns a value to a variable in the global environment or saves data to disk. Can be used to define new variables or update existing variables.

```clj
(set x 123)
123

x
123

(set price 150.25)
150.25
```

When the first argument is a symbol, sets a global variable.

!!! note ""
    `set` creates or updates global variables that persist across the session. For local variables within functions, use `let` instead.

### :material-code-brackets: Let

Creates a local variable scoped to the current block or function. The variable is only available within the scope where it's defined.

```clj
(do (let a 123) (+ a 1))
124
```

```clj
(set add (fn [a b] (let c (+ a b)) c))
(add 1 2)
3

(set calculate-total (fn [price quantity] 
    (let total (* price quantity))
    (let tax (* total 0.1))
    (+ total tax)))
(calculate-total 150.25 100)
16527.50
```

Unlike `set`, variables created with `let` are not accessible outside their defining scope. This is useful for temporary variables within functions or blocks.

!!! note ""
    `let` is a special form that restricts variable scope. Use `let` inside functions to avoid polluting the global environment.

## Variable Retrieval

### :material-database-search: Get

Retrieves a variable value by symbol or loads a serialized object from disk.

```clj
(set a 1)
(get 'a)
1
```

When given a symbol, returns the value of that variable from the environment.

## Environment Introspection

### :material-code-braces: Env

Returns a [:material-code-braces: Dictionary](data-types/dictionary.md) containing all global variables in the current environment.

```clj
(set add (fn [a b] (return (+ a b)) 77))
(set a 1)
(set b [1 2 3 4])
(set price 150.25)
(env)
{
    add: @add
    a: 1
    b: [1 2 3 4]
    price: 150.25
}
```

Returns a dictionary mapping variable names (as symbols) to their values.
