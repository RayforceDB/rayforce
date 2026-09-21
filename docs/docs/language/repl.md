# REPL Reference

Commands, profiling, keyboard shortcuts, and script mode for the Rayfall REPL.

## Starting the REPL

The `rayforce` binary operates in three modes depending on how it is invoked:

```bash
# Interactive mode — full line editing, highlighting, completion
./rayforce

# Run a script file
./rayforce script.rfl

# Pipe mode — reads from stdin, no colour output
echo '(+ 1 2)' | ./rayforce
```

Interactive mode is detected automatically when stdin is a terminal. On startup the REPL prints a banner with the version, CPU, memory, logical CPU count, and worker count, then shows the `‣` prompt. By default the pool includes one worker per **physical** core, excluding SMT siblings: two SMT threads share one core's issue bandwidth, so the second runs the same work at roughly half the IPC rather than adding throughput. Use `-c N` to choose a total worker count explicitly — worth doing on a latency-bound service on a low core-count SMT host, where the wider pool can still win wall-clock time at a higher CPU cost. Individual operations may use fewer tasks when their workload or memory budget requires it.

## REPL Commands

All commands start with `:` and are handled before expression evaluation.

| Command | Alias | Description |
|---|---|---|
| `:?` | `:help` | Display the command list. |
| `:t` | `:timeit` | Toggle profiling on/off. |
| `:env` |  | List all defined variables with their types. |
| `:clear` |  | Clear the screen. |
| `:q` | `:quit` | Exit the REPL. |

You can also exit with `\\`, `exit`, or **Ctrl-D** on an empty line.

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

Pipe mode works the same way but reads from stdin line by line, evaluating each balanced expression as it arrives.
