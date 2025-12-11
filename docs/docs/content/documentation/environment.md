# :material-code-braces: Environment Functions

RayforceDB provides functions for managing variables and the execution environment. These functions enable variable assignment, scoping, and environment introspection.

### Set

Assigns a value to a variable in the global environment or saves data to disk. Can be used to define new variables, update existing variables, or persist data to files.

```clj
↪ (set x 123)
123
↪ x
123
↪ (set "/tmp/v.dat" [1 2 3])
"/tmp/v.dat"
```

When the first argument is a symbol, sets a global variable. When the first argument is a string path, saves the value to disk as a serialized object.

!!! note ""
    `set` creates or updates global variables that persist across the session. For local variables within functions, use `let` instead.

### Let

Creates a local variable scoped to the current block or function. The variable is only available within the scope where it's defined.

```clj
↪ (do (let a 123) (+ a 1))
124
↪ a
;; Error: undefined symbol 'a
```

```clj
↪ (set add (fn [a b] (let c (+ a b)) c))
↪ (add 1 2)
3
```

Unlike `set`, variables created with `let` are not accessible outside their defining scope. This is useful for temporary variables within functions or blocks.

!!! note ""
    `let` is a special form that restricts variable scope. Use `let` inside functions to avoid polluting the global environment.

### Get

Retrieves a variable value by symbol or loads a serialized object from disk.

```clj
↪ (set a 1)
↪ (get 'a)
1
↪ (set "/tmp/vec" (til 10))
↪ (get "/tmp/vec")
[0 1 2 3 4 5 6 7 8 9]
```

When given a symbol, returns the value of that variable from the environment. When given a string path, reads and deserializes a file containing a RayforceDB object.

!!! note ""
    `get` searches the environment for variables. If the variable doesn't exist, it returns an error. For file paths, the file must contain a valid serialized RayforceDB object.

### Env

Returns a [:material-code-braces: Dictionary](data-types/dictionary.md) containing all global variables in the current environment.

```clj
↪ (set add (fn [a b] (return (+ a b)) 77))
↪ (set a 1)
↪ (set b [1 2 3 4])
↪ (env)
{add: @add, a: 1, b: [1 2 3 4]}
```

Returns a dictionary mapping variable names (as symbols) to their values. Useful for inspecting the current global state and debugging.

!!! note ""
    The returned dictionary is a snapshot of the global environment at the time of the call. Modifying the dictionary does not affect the actual environment variables.
