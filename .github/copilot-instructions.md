# Copilot Instructions for RayforceDB

This document provides guidance for GitHub Copilot when working with the RayforceDB codebase.

## Project Overview

RayforceDB is a high-performance columnar vector database written in pure C17. It features:
- Columnar storage with SIMD vectorized operations
- Custom parallel lock-free buddy allocator for memory management
- Minimal footprint (<1MB binary, zero dependencies)
- Cross-platform support (Linux, macOS, Windows, WebAssembly)
- Rayfall query language (Lisp-like syntax)

## Language and Standards

- **Language**: C17 standard (`-std=c17`)
- **Compiler**: Primary compiler is Clang, with GCC support for benchmarks
- **Character Type**: Use signed char (`-fsigned-char`)
- **Architecture**: Target is 64-bit (`-m64`), with native CPU optimizations (`-march=native`)

## Code Style and Formatting

### Formatting
- Follow the `.clang-format` configuration (Google style base with customizations):
  - 4-space indentation
  - 120 character line limit
  - Attach braces (K&R style)
  - Short blocks allowed on single line
  - Preserve include order (do not sort)

### Naming Conventions
- **Types**: Use `_t` suffix (e.g., `i64_t`, `f64_t`, `str_p`)
- **Functions**: Use lowercase with underscores (e.g., `hash_create`, `string_concat`)
- **Constants/Macros**: Use UPPERCASE with underscores (e.g., `DEBUG`, `NDEBUG`)

### Header Files
- All source files must include proper MIT license header
- Use include guards in header files: `#ifndef FILENAME_H` / `#define FILENAME_H` / `#endif`
- Organize includes: system headers first, then local headers
- Do not sort includes automatically - preserve existing order

## Architecture and Structure

### Core Components (in `core/` directory)
- **Memory Management**: `heap.c`, `freelist.c` - Custom allocator implementation
- **Runtime**: `runtime.c`, `eval.c`, `parse.c` - Rayfall interpreter
- **Data Types**: `temporal.c`, `date.c`, `time.c`, `timestamp.c`, `string.c`, `guid.c`
- **Operations**: `unary.c`, `binary.c`, `vary.c`, `aggr.c`, `math.c`
- **Query Engine**: `query.c`, `filter.c`, `join.c`, `group.c`, `index.c`
- **I/O & Networking**: `ipc.c`, `sock.c`, `io.c`, `fs.c`, `mmap.c`
- **Concurrency**: `thread.c`, `pool.c`, `atomic.c`
- **Platform Abstraction**: `os.c`, `sys.c`, `poll.c` (with `epoll.c`, `kqueue.c`, `iocp.c`)

### File Organization
- Main application: `app/main.c`
- Tests: `tests/main.c`
- Benchmarks: `bench/main.c`
- Extensions: `ext/` directory for plugins and bindings
- Examples: `examples/` directory for Rayfall (`.rfl`) scripts

## Build System

### Make Targets
- `make debug` - Debug build with sanitizers (default)
- `make release` - Optimized production build
- `make tests` - Build and run test suite
- `make bench` - Build and run benchmarks
- `make clean` - Clean build artifacts

### Platform-Specific Flags
- **Linux**: `-lpthread -lm -ldl`, uses `epoll` for I/O
- **macOS**: Includes sanitizers in debug builds (`-fsanitize=address -fsanitize=undefined`), uses `kqueue` for I/O
- **Windows**: Links against `ws2_32`, `mswsock`, `kernel32`, uses `iocp` for I/O

## Testing

- Test files are located in `tests/` directory
- Tests are compiled with `-DDEBUG` flag
- Run tests with: `./rayforce.test`
- Test examples are in `.rfl` files (e.g., `test_math_operations.rfl`, `test_filter_aggr.rfl`)

## Rayfall Query Language

Rayfall is a Lisp-like query language with S-expression syntax:

### Basic Syntax
```lisp
;; Comments start with semicolons
(function arg1 arg2 ...)

;; Arithmetic operations
(+ 1 2)                    ; => 3
(+ [1 2 3] 3)              ; => [4 5 6]
(* [1 2 3] [2 3 4])        ; => [2 6 12]

;; Aggregations
(sum [1 2 3])              ; => 6
(avg [1.0 2.0 3.0])        ; => 2.0
(min [1 2 3])              ; => 1
(max [1 2 3])              ; => 3

;; Date/Time operations
(+ 2024.03.20 5)           ; Add 5 days
(+ 09:00:00 3600)          ; Add seconds to time
```

### Data Types
- Integers: `1i`, `42i`
- Floats: `1.5`, `3.14`
- Dates: `2024.03.20`
- Times: `09:00:00`, `09:00:00.000`
- Timestamps: Combination of date and time
- Vectors: `[1 2 3]`
- Strings: Standard string literals

## Performance Considerations

### SIMD Vectorization
- Code is optimized for SIMD operations with vectorized loops
- Use compiler flags: `-ftree-vectorize -funsafe-math-optimizations -funroll-loops`
- Enable lax vector conversions: `-flax-vector-conversions`

### Memory Management
- Use custom allocator functions from `heap.c`
- Be mindful of memory alignment for SIMD operations
- Profile memory usage in performance-critical paths

### Optimization Flags
- Release builds use `-O3` optimization
- Math optimizations: `-fassociative-math -fno-math-errno`
- Platform-specific optimizations: `-march=native`

## Cross-Platform Development

### Platform Detection
- OS detection is automatic via Makefile
- Platform-specific code uses conditional compilation
- Support Linux, macOS, Windows, and WebAssembly targets

### I/O Multiplexing
- Linux: `epoll.c`
- macOS: `kqueue.c`
- Windows: `iocp.c`
- Use the `poll.c` abstraction layer for portable code

## Common Patterns

### Error Handling
- Check return values and handle errors appropriately
- Use error codes and status returns
- See `error.c` and `error.h` for error handling utilities

### String Operations
- Use custom string utilities from `string.c`
- Be aware of memory ownership and allocation

### Concurrency
- Thread pool implementation in `pool.c`
- Atomic operations in `atomic.c`
- Be thread-safe when accessing shared data structures

## Documentation

- Keep documentation up to date in `docs/` directory
- Provide examples in `examples/` directory
- Test reports: https://singaraiona.github.io/rayforce/tests_report/
- Coverage reports: https://singaraiona.github.io/rayforce/coverage_report/
- Full documentation: https://singaraiona.github.io/rayforce/

## Development Workflow

1. Make changes in appropriate `core/`, `app/`, or `tests/` files
2. Format code with `clang-format` following `.clang-format` rules
3. Build with `make debug` to catch issues early
4. Run tests with `make tests`
5. Verify release build with `make release`
6. Add examples in `examples/` directory if adding new features

## Additional Notes

- Minimize dependencies - the project aims for zero external dependencies
- Prioritize performance and memory efficiency
- Maintain cross-platform compatibility
- Follow the existing code structure and patterns
- Document complex algorithms and data structures
