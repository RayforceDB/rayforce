# :material-cog: Miscellaneous Functions

RayforceDB provides various utility functions for introspection, memory management, timing, and code loading. These functions enable debugging, performance measurement, and extensibility.

### Type

Returns the type of an object as a symbol.

```clj
↪ (type [1 2 3])
Symbol
↪ (type "hello")
C8
↪ (type 42)
I64
↪ (type true)
B8
```

### Count

Returns the number of elements in a collection or the length of a vector.

```clj
↪ (count [1 2 3])
3
↪ (count (list "a" "b" "c"))
3
↪ (count "hello")
5
```

### Reference Count

Returns the reference count of an object (minus one for the current reference). Useful for debugging memory management.

```clj
↪ (set x [1 2 3])
↪ (rc x)
1
↪ (set y x)
↪ (rc x)
2
```

!!! note ""
    Shows how many references point to an object. Objects are freed when reference count reaches 0. This is primarily a debugging tool.

### GC

Triggers garbage collection manually. Returns the number of objects collected.

```clj
↪ (gc)
42
```

!!! note ""
    Garbage collection runs automatically, but manual triggering can be useful for memory management in specific scenarios.

### Memstat

Returns memory statistics for the current RayforceDB process.

```clj
↪ (memstat)
{msys: 67338240, heap: 67108864, free: 652, syms: 177}
```

Returns a dictionary with the following keys:
- `msys` - System memory used (in bytes)
- `heap` - Total heap memory (in bytes)
- `free` - Free heap memory (in bytes)
- `syms` - Number of symbols in the symbol table

### Meta

Returns metadata about a [:material-table: Table](data-types/table.md), including column names, types, memory models, and attributes.

```clj
↪ (set t (table [name age] (list (list "Alice" "Bob") [25 30])))
↪ (meta t)
```

Returns a table with columns: `name`, `type`, `mmod`, and `attrs` describing the table structure.

### Load Function

Loads a function from a shared library (DLL/so) for extending RayforceDB functionality.

```clj
↪ (set f (loadfn "ext/libexample.so" "myfn" 2))
@fn
↪ (f 1 2)
3
```

Takes three arguments: the path to the shared library, the function name, and the number of arguments the function expects.

!!! note ""
    Requires creating a shared library with functions following RayforceDB's object interface. See the full documentation for details on creating plugins.

### Timer

Creates and manages recurring timers that execute callback functions at specified intervals.

```clj
↪ (timer 1000 0 (fn [t] (println "Timer tick:" t)))
0
↪ (timer 1000 5 (fn [t] (println "Count:" t)))
1
↪ (timer 0)
null
```

Takes three arguments: interval in milliseconds, number of times to execute (0 for infinite), and a callback lambda. Returns a timer ID. To remove a timer, call `timer` with just the timer ID.

!!! note ""
    The callback function receives the current time as an argument. Timers continue until removed or the execution count is reached.

### Timeit

Measures the execution time of an expression in milliseconds.

```clj
↪ (timeit (+ 1 2))
0.015
↪ (timeit (map (fn [x] (* x 2)) [1 2 3 4 5]))
0.245
↪ (timeit 1000 (expensive-calculation))
```

Can take one argument (expression to measure) or two arguments (number of iterations and expression to measure repeatedly).

!!! note ""
    Returns execution time in milliseconds. Results may vary between runs due to system load and timer resolution.
