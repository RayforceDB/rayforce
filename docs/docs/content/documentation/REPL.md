# :material-console: REPL Functions

RayforceDB provides functions for parsing, evaluating, and loading code, as well as controlling REPL behavior and formatting. These functions enable code introspection, dynamic execution, and customization of the interactive environment.

### Parse

Parses a string into an expression without evaluating it. Returns the parsed abstract syntax tree.

```clj
(parse "(+ 1 2)")
(
  +
  1
  2
)

(parse "(fn [x] (* x 2))")
@(null)
```

### Eval

Evaluates a string or expression. When given a string, parses and evaluates it. When given an expression, evaluates it directly.

```clj
(eval "(+ 1 2)")
3

(set expr (list + 1 2))
(eval expr)
3
```

### Load

Loads and evaluates a RayforceDB source file. If the path ends with a slash, loads a table from that directory. Otherwise, reads the file as code and evaluates it.

```clj
(load "myfile.rfl")

(load "/path/to/table/")
```

When loading a table directory, the table is bound to a symbol derived from the directory name.

### Args

Returns the command-line arguments passed to the RayforceDB process.

```clj
(args)
["arg1" "arg2" "arg3"]
```

### Quote

Prevents evaluation of an expression, returning it as-is. This is a special form.

```clj
(quote (+ 1 2))
(
  +
  1
  2
)

(quote x)
x
```

### Resolve

Resolves a symbol to its value in the current environment. Returns the value if found, `null` otherwise.

```clj
(set x 42)
(resolve 'x)
42

(resolve 'nonexistent)
null
```

### Display Width

Sets the maximum display width for table output formatting. Use the command `:set-display-width` in the REPL.

```clj
:set-display-width 120
```

Controls how wide tables can be before wrapping or truncating columns.

### FPR

Sets the floating-point precision for number formatting. Use the command `:set-fpr` in the REPL.

```clj
:set-fpr 6
```

Controls how many decimal places are shown when displaying floating-point numbers.

### Use Unicode Format

Enables or disables Unicode characters for table formatting. Use the command `:use-unicode` in the REPL.

```clj
:use-unicode 1
:use-unicode 0
```

When enabled, uses Unicode box-drawing characters for tables. When disabled, uses ASCII characters.

## Input-Output Operations

### :material-database-search: Get

Gets a variable by symbol or reads a serialized object from a file path.

```clj
(get 'myvar)
[1 2 3]

(get "/path/to/file")
[1 2 3]
```

When called with a symbol, returns the value of that variable. When called with a string path, reads and deserializes a file containing a RayforceDB object.

### :material-file-document: Read

Reads a file from disk. When given a file path string, returns the file contents as a string. When given a file handle, reads IPC messages.

```clj
(read "/path/to/file.txt")
"file contents"

(read handle)
```

For file handles, returns a dictionary with `items`, `read`, and `total` keys indicating how many messages were read.

### :material-file-edit: Write

Writes data to a file handle or IPC connection.

```clj
(write handle "(+ 1 2)")
3

(write handle (list (+ 1 2)))
3

(write fd data)
```

When writing to an IPC handle, the data is sent as a message and evaluated on the remote side. When writing to a file handle, the data is serialized and written to the file.

## Utility Functions

### :material-code-tags: Type

Returns the type of an object as a symbol.

```clj
(type [1 2 3])
Symbol

(type "hello")
C8

(type 42)
I64

(type true)
B8

(type 150.25)
F64
```

### :material-counter: Count

Returns the number of elements in a collection or the length of a vector.

```clj
(count [1 2 3])
3

(count (list "a" "b" "c"))
3

(count "hello")
5

(count [150.25 300.50 125.75])
3
```

### :material-memory: Reference Count

Returns the reference count of an object (minus one for the current reference). Useful for debugging memory management.

```clj
(set x [1 2 3])
(rc x)
1

(set y x)
(rc x)
2
```

!!! note ""
    Shows how many references point to an object. Objects are freed when reference count reaches 0. This is primarily a debugging tool.

### :material-delete-sweep: GC

Triggers garbage collection manually. Returns the number of objects collected.

```clj
(gc)
42
```

!!! note ""
    Garbage collection runs automatically, but manual triggering can be useful for memory management in specific scenarios.

### :material-memory: Memstat

Returns memory statistics for the current RayforceDB process.

```clj
(memstat)
{msys: 67338240, heap: 67108864, free: 652, syms: 177}
```

!!! note ""
    - `msys` - System memory used (in bytes)
    - `heap` - Total heap memory (in bytes)
    - `free` - Free heap memory (in bytes)
    - `syms` - Number of symbols in the symbol table

### :material-information: Meta

Returns metadata about a [:material-table: Table](../data-types/table.md), including column names, types, memory models, and attributes.

```clj
(set t (table [name age] (list (list "Alice" "Bob") [25 30])))
(meta t)
┌──────┬──────┬──────┬─────────────────┐
│ name │ type │ mmod │ attrs           │
├──────┼──────┼──────┼─────────────────┤
│ name │ List │ 255  │ 0               │
│ age  │ I64  │ 255  │ 0               │
├──────┴──────┴──────┴─────────────────┤
│ 2 rows (2 shown) 4 columns (4 shown) │
└──────────────────────────────────────┘
```

### :material-puzzle: Load Function

Loads a function from a shared library (DLL/so) for extending RayforceDB functionality.

```clj
(set f (loadfn "ext/libexample.so" "myfn" 2))
@fn

(f 1 2)
3
```

Takes three arguments: the path to the shared library, the function name, and the number of arguments the function expects.

!!! note ""
    Requires creating a shared library with functions following RayforceDB's object interface.

### :material-timer: Timer

Creates and manages recurring timers that execute callback functions at specified intervals.

```clj
(timer 1000 0 (fn [t] (println "Timer tick:" t)))
0

(timer 1000 5 (fn [t] (println "Count:" t)))
1

(timer 0)
null
```

Takes three arguments: interval in milliseconds, number of times to execute (0 for infinite), and a callback lambda. Returns a timer ID. To remove a timer, call `timer` with just the timer ID.

!!! note ""
    The callback function receives the current time as an argument. Timers continue until removed or the execution count is reached.

### :material-timer-outline: Timeit

Measures the execution time of an expression in milliseconds.

```clj
(timeit (+ 1 2))
0.015

(timeit (map (fn [x] (* x 2)) [1 2 3 4 5]))
0.245

(timeit 1000 (expensive-calculation))
```

Can take one argument (expression to measure) or two arguments (number of iterations and expression to measure repeatedly).

!!! note ""
    Returns execution time in milliseconds.

## REPL Commands

The REPL supports several built-in commands prefixed with `:`:

- `:?` - Displays help and list of available commands
- `:u [0|1]` - Toggle Unicode formatting for tables
- `:t [0|1]` - Toggle time measurement of expressions
- `:q [exit code]` - Exit the application with optional exit code
