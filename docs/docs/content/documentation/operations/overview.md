# :material-calculator: Operations Overview

RayforceDB provides a comprehensive set of operations for data manipulation and analysis.

## Scalar Values

A scalar is a single atomic value, as opposed to a vector or collection. Scalars include:

- **Integers** - `I8`, `I16`, `I32`, `I64` (signed) and `U8`, `U16`, `U32`, `U64` (unsigned)
- **Floats** - `F32`, `F64` for floating-point numbers
- **Temporal** - Dates, times, timestamps, and timespans
- **Symbols** - Interned strings for efficient comparison
- **Characters** - Single character values
- **Booleans** - `true` and `false`
- **GUIDs** - Globally unique identifiers

## Operation Categories

### [Math Operations](./math.md)
Arithmetic, aggregation, and mathematical functions for numeric data.

### [Order Operations](./order.md)
Sorting, ranking, and ordering functions.

### [Iterable Operations](./iterable.md)
Functions for working with collections and sequences.

### [Logic Operations](./logic.md)
Boolean and comparison operations.

### [Compose Operations](./compose.md)
Function composition and higher-order operations.
