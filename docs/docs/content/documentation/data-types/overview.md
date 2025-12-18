# :simple-databricks: Data Types

RayforceDB supports several categories of data types. Each category serves a specific purpose in the database system:


## [:octicons-check-16: B8](./boolean.md)

!!! note ""
    Boolean value representing true or false.

## [:octicons-number-16: Integers](./integers.md)

!!! note ""
    8-bit unsigned integer and 16/32/64-bit signed integers.

## [:material-text: Symbol](./symbol.md)

!!! note ""
    Interned string used for efficient string storage and comparison.

## [:octicons-clock-16: Temporal](./temporal.md)

!!! note ""
    Date (YYYY.MM.DD), Time (HH:MM:SS.mmm), and Timestamp (YYYY.MM.DD\DHH:MM:SS.mmm) formats.

## [:material-decimal: F64](./float.md)

!!! note ""
    64-bit signed floating-point number.

## [:material-identifier: GUID](./guid.md)

!!! note ""
    Globally Unique Identifier for unique identification of objects.

## [:material-format-letter-case: C8](./char.md)

!!! note ""
    Single character value.


## [:material-vector-line: Vector](./vector.md)

!!! note ""
    Represents a collection of elements of a certain type, enabling efficient storage and operations on homogeneous data.

## [:material-text-shadow: String](./string.md)

!!! note ""
    Represents a [:material-vector-line: Vector](./vector.md) of [C8](./char.md) (characters), used for storing text values.

## [:material-code-array: List](./list.md)

!!! note ""
    Represents a collection of elements that are not necessarily of the same type.

## [:material-table: Table](./table.md)

!!! note ""
    An object that holds information about the columns and rows of a specific dataset, forming the core structure for relational operations.

## [:material-code-braces: Dictionary](./dictionary.md)

!!! note ""
    An object that holds information about keys and their corresponding values.

## [:material-function: Function](./functions.md)

!!! note ""
    Built-in or custom user-defined functions and lambdas that perform operations on data.

## [:material-numeric: Enum](./enum.md)

!!! note ""
    An enumeration of certain values, providing a way to represent a fixed set of named constants.

## [:material-alphabetical-variant: Symbols & Enums In-Depth](./symbols-and-enums.md)

!!! note ""
    Comprehensive guide explaining symbol interning, why enumerations are essential for storage, how symfiles work, and why shared symfiles are required for parted tables. **Recommended reading for newcomers.**

## [:material-dots-horizontal: Other](./other.md)

!!! note ""
    Special types including `Null` and `Error` used throughout the system.

---

## :octicons-code-24: What is a Type Code?

A type code is a specific integer that represents a particular data type. It's primarily used in internal object evaluations.
