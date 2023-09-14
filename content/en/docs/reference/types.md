---
title: "Types"
description: "Types of RayforceDB"
date: 2023-09-06T08:48:57+00:00
lastmod: 2023-09-06T08:48:57+00:00
draft: false
images: []
menu:
  docs:
    parent: "reference"
weight: 1
toc: true
---


{{< details "All the types are divided onto 3 categories:" open >}}

- Atoms
- Vectors
- Other

{{< /details >}}

Atoms are the types that can be stored in a single cell of a table. Vectors are the types that can be stored in a single cell of a table, but they are stored in a special way. Other types are the types that can't be stored in a single cell of a
table.

```go
fmt.Println("Hello, World!")
```

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
