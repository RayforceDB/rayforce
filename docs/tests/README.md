# Documentation Testing

The test script `tests/test_docs.py` automatically validates code examples in markdown documentation by executing them against the RayForce REPL and comparing outputs.

**Requirements:** `expect` command (installed by default on macOS/Linux)

### The `↪` Marker Convention

Lines prefixed with `↪` are **testable expressions**. The test framework will:
1. Execute the expression
2. Compare the result to the expected output (following lines until next `↪` or blank line)

```clj
;; TESTABLE - will be executed and validated
↪ (+ 1 2)
3

;; NOT TESTABLE - documentation only (no ↪ prefix)
(some-complex-example)
;; This just shows syntax, won't be tested
```

### When to Use `↪` (Testable)

Use `↪` for:
- Deterministic expressions with predictable output
- Core language features that should always work
- Examples that validate documented behavior

### When NOT to Use `↪`

Omit `↪` for:
- **Random/dynamic output**: `(rand ...)`, `(guid ...)`, `(timestamp 'local)`
- **Side-effect functions**: `(print ...)`, `(show ...)` - return `[]`, not printed output
- **Undefined variables**: Examples using hypothetical data like `trades`, `large_table`
- **Complex table output**


### Multiline Expressions

Expressions continue until parentheses balance:

```clj
↪ (select {name: name
           age: age
           from: users})
```

### Running Tests

```bash
cd docs

# Test all docs
python tests/test_docs.py

# Test specific file
python tests/test_docs.py docs/content/math/add.md

# Test multiple categories
python tests/test_docs.py docs/content/math/*.md docs/content/cmp/*.md

# Verbose output (show failures)
python tests/test_docs.py -v

# Extract examples without testing
python tests/test_docs.py -e

# Generate JSON report
python tests/test_docs.py --report
```

### Rationale

**Why test documentation?**
- Catches outdated examples when the language changes
- Ensures documented behavior matches actual behavior
- Prevents copy-paste errors in examples
- Serves as regression tests for core functionality

**Why the `↪` convention?**
- Explicit opt-in: only marked examples are tested
- Allows mixing testable code with conceptual examples
- Easy to disable problematic tests by removing marker
- Visual indicator to readers that "this actually works"