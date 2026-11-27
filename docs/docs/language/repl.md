# REPL Reference

Commands, profiling, keyboard shortcuts, and script mode for the Rayfall REPL.

## Starting the REPL

The `rayforce` binary operates in three modes depending on how it is invoked:

```bash
# Interactive mode — full line editing, highlighting, completion
./rayforce

# Run a script file
./rayforce script.rfl

# Run a script from stdin, a pipe or a FIFO
generate_script | ./rayforce -

# Pipe mode — reads from stdin, no colour output
echo '(+ 1 2)' | ./rayforce
```

Interactive mode is detected automatically when stdin is a terminal. On startup the REPL prints a banner with the version, CPU, memory, logical CPU count, and worker count, then shows the `‣` prompt. By default the pool has one thread per logical CPU the process may use (see [Pool Size](#pool-size)). Use `-c N` to choose a total worker count explicitly: N must contain decimal digits only; `0` selects the automatic size, and `1..1023` sets the total count, including the main thread. Values outside this range are rejected before the pool starts. Individual operations may use fewer tasks when their workload or memory budget requires it. Use `-m SIZE` (`--mem`; a byte count or a `K`/`M`/`G`/`T` suffix) to set the RAM budget: anonymous memory past it spills to disk instead of risking the OOM killer. `(.sys.mem)` reports the effective value as [`anon-watermark`](../namespaces/sys.md#sys-mem).

Under a cgroup CPU quota (`docker --cpus`, systemd `CPUQuota=`) the default pool has one thread per CPU of the quota, rounded up. A `-c` larger than the allowed CPUs prints a warning. See [Running Several Instances](../guides/instances.md) for choosing CPUs per instance.

### Pool Size

With no `-c`, or `-c 0`, and no `RAYFORCE_CORES` in the environment, the pool has one thread for every logical CPU the process may use, the main thread included. On Linux that is the CPUs of its affinity mask (every online CPU unless `taskset`, a cpuset or `docker --cpuset-cpus` narrowed it), and no more than its cgroup CPU quota, rounded up; elsewhere it is every online CPU. Both hardware threads of an SMT core count. A non-empty `RAYFORCE_CORES` (see [Environment Variables](#environment-variables)) replaces this size, with or without `-c 0`; only a positive `-c N` overrides it. The default suits large scans, grouping, sorts and joins, which keep every thread busy. It costs CPU time in two cases:

- **SMT machines.** The second thread of a core shares that core's load and store units, so it adds little wall time and a lot of CPU time. On a 4-core, 8-thread desktop a join probe took 0.70 s at the default and 0.67 s at `-c 4`, for twice the CPU time (5.17 s against 2.59 s). On an 8-core, 16-thread laptop it took 0.16 s at both `-c 8` and the default, which spent 1.7 times the cycles ([#599](https://github.com/RayforceDB/rayforce/issues/599)). Large analytic queries can still gain from the extra threads: over a 43-query benchmark on that desktop the per-query times were 2.7% shorter at the default than at `-c 4`.
- **Small or mostly single-threaded services.** A service whose work fits one thread gains nothing from the extra threads, and they still cost CPU time when its queries reach the pool. On an 8-core, 16-thread laptop one such service used 8% of a core at the default and 6% at `-c 1` or `-c 2`, for the same rows ([#1491](https://github.com/RayforceDB/rayforce/issues/1491)).

Without SMT the default costs little: on a 14-core laptop (10 performance and 4 efficiency cores) the default was never slower than `-c 10` beyond noise and was 5 to 9% faster on joins, partitioned scans and grouping 50M rows by 5M keys, for 14 to 31% more CPU time per query ([#1491](https://github.com/RayforceDB/rayforce/issues/1491)).

Set `-c N` for a service that is not bound by scans: the number of physical cores (`lscpu`: `Core(s) per socket` × `Socket(s)`) on an SMT machine, or 1 or 2 for a service that is mostly single-threaded and does not need its queries to finish sooner. `(.sys.info)` reports the pool as `workers`, the main thread included, and the CPUs the process may run on as `cores` (under a CPU quota `cores` still counts every allowed CPU):

```bash
rayforce -c 8 -p 5001
```

```lisp
(get (.sys.info) 'workers)   ; 8
```

To keep a large job fast while small joins stay on one thread, raise [`.sys.join.par.min`](../namespaces/sys.md#sys-join-par-min) instead of lowering `-c` (see [Parallel probe floor](../queries/joins.md#parallel-probe-floor)).

### Environment Variables

The thread pool reads these once, when it is created (process start, or the first parallel operation):

| Variable | Default | Description |
|---|---|---|
| `RAYFORCE_CORES` | unset | Number of background worker threads when no `-c` is given (the main thread is one more). Unset or empty keeps the [default size](#pool-size); `0`, or a value that does not start with a positive number, gives the main thread alone. It also applies under `-c 0`. The test harness sets it to keep spawned processes small. Only a positive `-c N` overrides it. |
| `RAY_POOL_STEAL` | `1` | How workers claim tasks. `1`: each worker owns a contiguous slice of the table and steals halves of other workers' remaining slices once its own is done — sequential access and a stable row-to-worker mapping. `0`: every worker claims the next task from one shared cursor. Results are identical; set `0` only to A/B the two schedulers. |
| `RAY_POOL_TRACE` | unset | `1` prints a per-worker histogram to stderr when the pool is destroyed (at exit): tasks each worker ran from its own slice versus stolen ones, and the number of steals. Use it to see how evenly work splits across workers on a given machine. |

## REPL Commands

All commands start with `:` and are handled before expression evaluation.

| Command | Alias | Description |
|---|---|---|
| `:?` | `:help` | Display the command list. |
| `:h NAME` | `:help NAME` | Show how a builtin or lambda is called (see [below](#help-for-a-name)). |
| `:t` | `:timeit` | Toggle profiling on/off. |
| `:env` |  | List all defined variables with their types. |
| `:clear` |  | Clear the screen. |
| `:q` | `:quit` | Exit the REPL. |

You can also exit with `\\`, `exit`, or **Ctrl-D** on an empty line.

## Help for a name { #help-for-a-name }

`:h NAME` (or `:help NAME`) prints what [`.repl.signature`](../namespaces/repl.md#repl-signature) reports for `NAME`, formatted for reading. It resolves the name by the same rules: a global, or a dotted path through dictionary members. Nothing is evaluated or called.

- **A builtin:** its documented call forms, one per line, from the engine's usage table (the text its errors cite), then its one-line summary. Through an alias, the header names the builtin the alias holds.
- **A lambda:** its call shape from its parameter names.
- **Flags:** `special form` and `restricted under -U` are added where they apply. Then how it treats a collection, from `.repl.signature`'s `atomic`, `elementwise` and `aggregate`: `atomic`; or `element-wise`, or `element-wise in argument 1` when only some arguments map; and `aggregate`. `element-wise` is the row-shape record queries plan by, so for `if` it describes a query: an ordinary `if` with a boolean-vector condition needs an else arm (see [`.repl.signature`](../namespaces/repl.md#repl-signature)).
- **Anything else:** one line: a namespace root (`:h .sys`) gives its number of names and how to ask for one, `true` and `false` are literals, and a value or a missing name says so.

```
‣ :h min
min: builtin (unary), aggregate
  (min <x: vector, atom or list>)
  (min <a: num> <b: num>)
  Reduce one vector, or choose the element-wise minimum of two values, in their promoted type (null lowest); f32 input answers f32
‣ (set add (fn [a b] (+ a b)))
‣ :h add
add: lambda
  (add a b)
‣ :h if
if: builtin (vary), special form, element-wise
  (if <cond: bool or bool vector> <then> [<cond> <then> ...] [<else>])
  Conditional: (if c1 r1 c2 r2 ... else?) — first truthy condition wins
‣ :h .ipc.close
.ipc.close: builtin (unary), restricted under -U
  (.ipc.close <handle: i32 or i64>)
  Close an IPC connection handle
‣ :h nosuch
nosuch: not defined
```

Bare `:?`, `:help` and `:h` list the commands. Scripts and editors ask `.repl.signature` itself, which returns the same facts as a dict.

## Profiling with :t

Toggle profiling with `:t`. When active, every evaluation emits a nested span tree showing where time was spent:

```
‣ :t
. Timeit is on.
‣ (+ 1 2)
3
╭ top-level
│ ✶  parse: 0.016 ms
│ ✶  eval: 0.004 ms
╰─┤ 0.025 ms
‣ :t
. Timeit is off.
```

Spans nest automatically. The DAG executor reports sub-spans for optimizer passes (type inference, fusion, predicate pushdown) and per-morsel execution when applicable. A progress bar appears for long-running operations.

For one-off measurement, use the `timeit` builtin which returns elapsed time in milliseconds:

```lisp
(timeit (+ 1 2))
;; => 0.003
```

## Multi-Line Input

The REPL auto-detects incomplete expressions by tracking unmatched brackets `( [ {`. When the current input has unmatched openers, pressing Enter starts a continuation line instead of evaluating:

```text
‣ (set double
    (fn [x]
      (* x 2)))
; evaluated after the closing ) balances all openers
```

The parser is aware of string literals and `;` comments, so brackets inside strings or comments do not affect the count.

## Keyboard Shortcuts

| Key | Action |
|---|---|
| **Up** / **Ctrl-P** | Previous history entry |
| **Down** / **Ctrl-N** | Next history entry |
| **Left** / **Ctrl-B** | Move cursor left |
| **Right** / **Ctrl-F** | Move cursor right (at end of line accepts ghost text) |
| **Alt-B** / **Ctrl-Left** / **Alt-Left** | Move one word left |
| **Alt-F** / **Ctrl-Right** / **Alt-Right** | Move one word right |
| **Home** / **Ctrl-A** | Move to start of line |
| **End** / **Ctrl-E** | Move to end of line |
| **Tab** | Autocomplete (cycles through candidates) |
| **Ctrl-R** | Reverse incremental history search |
| **Ctrl-K** | Kill from cursor to end of line |
| **Ctrl-L** | Clear and redraw the screen |
| **Ctrl-T** | Transpose characters around the cursor |
| **Ctrl-U** | Kill entire line |
| **Ctrl-W** | Delete word backward |
| **Alt-D** | Delete word forward |
| **Alt-Backspace** | Delete word backward |
| **Delete** | Delete character at cursor |
| **Backspace** | Delete character before cursor |
| **Ctrl-C** | Cancel current input (interrupts running evaluation) |
| **Ctrl-D** | Exit on empty line, otherwise delete character at cursor |
| **Esc** | Cancel tab-completion cycling |

## Syntax Highlighting

The REPL highlights input in real time as you type:

- **Keywords** and special forms (`set`, `fn`, `select`, `if`, ...)
- **Strings** in double quotes
- **Standalone operators** (`+`, `-`, `*`, `/`, comparisons, ...)
- **Comments** starting with `;`
- **Bracket matching** — the bracket under the cursor and its partner are highlighted; unmatched delimiters are marked in red.

Highlighting is stripped from the final submitted text so it does not interfere with parsing.

## Autocomplete

Press **Tab** to complete the current token. Candidates are drawn from four sources:

- Built-in function names
- Global environment bindings (variables you have defined)
- Table column names when the current query references a table via `from:` or `from: 'table`
- Words from history entries

Press **Tab** repeatedly to cycle through matches. While cycling, the REPL shows a compact candidate strip with source labels such as `[env]`, `[col]`, and `[hist]`. Press **Esc** to cancel.

## Script Mode

Pass a `.rfl` file path to run it non-interactively:

```bash
./rayforce analysis.rfl
```

The entire file is read into memory and evaluated as a single Rayfall program via `ray_eval_str()`. The last expression's value is printed to stdout. If evaluation produces an error, it is printed to stderr and the process exits with code **1**; otherwise the exit code is **0**.

The file does not have to be a regular file: a pipe, a FIFO or process substitution (`./rayforce <(generate_script)`) is read to EOF, and `-` as the file name reads stdin (`./rayforce /dev/stdin` does the same). A script read this way runs as a file does, and prints the value of its last expression. That differs from piping into the REPL with no file argument (`echo '(+ 1 2)' | ./rayforce`), which evaluates the input expression by expression and prints each value. A script is at most 256 MiB, the same for a file and a stream, and holds at most 256 top-level forms: wrap a longer sequence in `(do ...)`, or run it through the REPL pipe.

Pipe mode works the same way but reads from stdin line by line, evaluating each balanced expression as it arrives.
