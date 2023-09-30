# :material-dna: Types

All the types are divided onto 3 categories:

- Atoms
- Vectors
- Other

Atoms are the types that are stored in a single "cell" like a register in a CPU world. Vectors are the lists of atoms, literally, contiguous arrays. Other types are functions, compounds, etc.

!!! note
    There is no separate type for `Null`. It cannot be explicitly created and has no "physical" representation, as it's merely a null pointer.

List of atoms called vectors. Vectors are contiguous arrays of atoms. They are the most used data structure in RayforceDB. They are used to store data in tables, dictionaries, and other data structures. Such way of storing data is very efficient and allows to use [SIMD](https://uk.wikipedia.org/wiki/SIMD) instructions to speed up the processing. Vectors have positive type ids of it's atoms, for example: if atom id is -3 (i64), then vector id is 3 (I64). Typenames, starting from capital letter, are used to denote vectors, for example: `I64` is a vector of `i64` atoms.

## Reference card

| Id   |  Name       | Size  |  Description                        |
| ---  |   ---       |  ---  |      ---                            |
|  0   | `List`      |   -   |  Generic List                       |
|  1   | `Bool`      |   1   |  Boolean                            |
|  2   | `Byte`      |   1   |  Byte                               |
|  3   | `I64`       |   8   |  Signed 64-bit Integer              |
|  4   | `F64`       |   8   |  64-bit Floating Point              |
|  5   | `Symbol`    |   8   |  Symbol (interned string)           |
|  6   | `Timestamp` |   8   |  Timestamp                          |
|  7   | `Guid`      |   16  |  Globally Unique Identifier         |
|  8   | `Char`      |   4   |   Character                         |
|  20  | `Enum`      |   -   |  Enumerated Type                    |
|  77  | `AnyMap`    |   -   |  Generic Map                        |
|  78  | `VecMap`    |   -   |  Vector Map                         |
|  79  | `ListMap`   |   -   |  List Map                           |
|  98  | `Table`     |   -   |  Table                              |
|  99  | `Dict`      |   -   |  Dictionary                         |
|  100 | `Lambda`    |   -   |  Lambda (user function)             |
|  101 | `Unary`     |   -   |  Unary (function with 1 argument)   |
|  102 | `Binary`    |   -   |  Binary (function with 2 arguments) |
|  103 | `Vary`      |   -   |  Vary (function with n arguments)   |
|  127 | `Error`     |   -   |  Error (special type for errors)    |
