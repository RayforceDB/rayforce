# Quick Start

Build Rayforce from source, start the Rayfall REPL, and run your first query in under 5 minutes.

## Prerequisites

Rayforce is a pure C library with zero external dependencies. You need:

- A **C compiler** — GCC 7+ or Clang 6+
- **GNU Make**
- **POSIX environment** (Linux, macOS, WSL)

## Build from Source

Clone the repository and build:

```bash
git clone https://github.com/RayforceDB/rayforce.git
cd rayforce
```

### Debug Build (default)

The default build enables AddressSanitizer (ASan) and UndefinedBehaviorSanitizer (UBSan) for maximum safety during development:

```bash
make
```

### Release Build

Optimized build with `-O3` and no sanitizers:

```bash
make release
```

### Run Tests

Rayforce ships with a comprehensive test suite covering every subsystem:

```bash
# Run all tests
make test

# Run only the tests whose name contains a substring
./rayforce.test -f vec

# e.g. just the language tests
./rayforce.test -f lang
```

## Start the REPL

The Rayfall REPL provides an interactive environment with syntax highlighting, bracket matching, tab completion, and multi-line input:

```bash
./rayforce
```

You will see the `‣` prompt (a green triangle bullet; `►` on Windows, whose console fonts have no `‣`):

```text
‣
```

## Your First Table

Create a table with two columns using the `table` function. Column names are symbols (quoted identifiers), and column data are vectors:

```lisp
‣ (set t (table [x y] (list [1 2 3] [A B C])))
```

```text
┌─────┬───────────────────────────────┐
│  x  │               y               │
│ I64 │              SYM              │
├─────┼───────────────────────────────┤
│ 1   │ A                             │
│ 2   │ B                             │
│ 3   │ C                             │
├─────┴───────────────────────────────┤
│ 3 rows (3 shown) 2 columns (2 shown)│
└─────────────────────────────────────┘
```

## Your First Query

Use `select` to query the table. The `where:` clause filters rows, and you can project specific columns:

```lisp
‣ (select {from:t where: (> x 1)})
```

```text
┌─────┬───────────────────────────────┐
│  x  │               y               │
│ I64 │              SYM              │
├─────┼───────────────────────────────┤
│ 2   │ B                             │
│ 3   │ C                             │
├─────┴───────────────────────────────┤
│ 2 rows (2 shown) 2 columns (2 shown)│
└─────────────────────────────────────┘
```

Add computed columns and aggregations:

```lisp
; Select with a computed column
‣ (select {from:t x:x x2: (* x x)})
```

```text
┌─────┬───────────────────────────────┐
│  x  │              x2               │
│ I64 │              I64              │
├─────┼───────────────────────────────┤
│ 1   │ 1                             │
│ 2   │ 4                             │
│ 3   │ 9                             │
├─────┴───────────────────────────────┤
│ 3 rows (3 shown) 2 columns (2 shown)│
└─────────────────────────────────────┘
```

```lisp
; Group by and aggregate
‣ (select {from:t by: y total: (sum x)})
```

```text
┌─────┬───────────────────────────────┐
│  y  │             total             │
│ SYM │              I64              │
├─────┼───────────────────────────────┤
│ A   │ 1                             │
│ B   │ 2                             │
│ C   │ 3                             │
├─────┴───────────────────────────────┤
│ 3 rows (3 shown) 2 columns (2 shown)│
└─────────────────────────────────────┘
```

## Load a CSV File

Load data from CSV files with automatic type inference, parallel parsing, and null handling:

```lisp
‣ (write "/tmp/rayforce-trades.csv" "sym,price,qty\nAAPL,150.25,100\nMSFT,420.0,50\n")
‣ (set data (.csv.read "/tmp/rayforce-trades.csv"))
‣ (select {from:data where: (> price 100)})
```

## Run a Script

Rayfall scripts use the `.rfl` extension. Run them directly from the command line:

```bash
# Run an example script
./rayforce examples/rfl/pivot.rfl

# Run your own script
./rayforce my-analysis.rfl
```

### Passing arguments to a script

Anything after a `--` separator is handed to your script instead of being read as a Rayforce flag. Read them from Rayfall with [`(.sys.args)`](../namespaces/sys.md#sys-args), which returns a dict: the launcher flags at the top level (typed) and your own options under `user`.

```bash
./rayforce my-analysis.rfl -- --symbol AAPL --limit 100
```

```lisp
(at (.sys.args) 'user)
;; => {symbol:"AAPL" limit:"100"}
```

User values are always strings — cast them as needed (e.g. `(as 'i64 (get (at (.sys.args) 'user) 'limit))` → `100`). A bare flag (no following value) maps to the empty string.

## REPL Commands

The REPL supports several built-in commands:

| Command | Description |
|---|---|
| `:help` | Show available commands |
| `:timeit` (alias `:t`) | Toggle profiling on/off (or `:t 0`/`:t 1`) |
| `:env` | List all global bindings |
| `:clear` | Clear the screen |
| `:quit` | Exit the REPL |

## What's Next

- [**Syntax & Types**](../language/syntax.md) — Learn the full Rayfall language
- [**Functions Reference**](../language/functions.md) — Browse the current built-in function surface
- [**Joins**](../language/functions.md#joins) — Left joins, inner joins, window joins, and as-of joins
