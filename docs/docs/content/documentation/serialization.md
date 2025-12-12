# :material-package: Serialization

RayforceDB provides serialization functions to convert data structures into binary formats suitable for storage, transmission, or persistence. Serialization enables data persistence, inter-process communication, and data exchange between RayforceDB instances.

Serialization converts nearly any RayforceDB data type into a byte array format that preserves type information and structure. The serialized format includes a header with version information and type metadata, making it self-describing and version-aware.

## Serialization

### :material-code-braces: Serialize `ser`

Converts a RayforceDB value into a binary byte array format. The serialized data includes type information and preserves the complete structure of complex objects.

```clj
(ser 42)
[0xfa 0xde 0xfa 0xce 0x01 0x00 0x00 0x00 0x09 0x00 0x00 0x00 0x00 0x00 0x00 0x00..]

(ser "hello")
[0xfa 0xde 0xfa 0xce 0x01 0x00 0x00 0x00 0x0f 0x00 0x00 0x00 0x00 0x00 0x00 0x00..]

(ser [1 2 3])
[0xfa 0xde 0xfa 0xce 0x01 0x00 0x00 0x00 0x22 0x00 0x00 0x00 0x00 0x00 0x00 0x00..]

(ser [150.25 300.50 125.75])
[0xfa 0xde 0xfa 0xce...]

(ser (table [symbol price] (list ['AAPL 'MSFT 'GOOG] [150.25 300.50 125.75])))
[0xfa 0xde 0xfa 0xce...]

(ser (dict ['symbol 'price] ['AAPL 150.25]))
[0xfa 0xde 0xfa 0xce...]
```

The serialized output is a vector of bytes (U8) that can be stored to disk, sent over a network, or used with IPC communication.

!!! note ""
    Functions are serialized as their source code. When deserializing functions, ensure all dependencies are available in the runtime environment.

## Deserialization

### :material-package-variant: Deserialize `de`

Reconstructs a RayforceDB value from a serialized byte array. Restores the original type and structure of the serialized object.

```clj
(de (ser 42))
42

(de (ser "hello"))
"hello"

(de (ser [1 2 3]))
[1 2 3]

(de (ser [150.25 300.50 125.75]))
[150.25 300.50 125.75]

(de (ser (dict ['a 'b] [1 2])))
{a: 1, b: 2}

(de (ser (table [symbol price] (list ['AAPL 'MSFT] [150.25 300.50]))))
┌────────┬────────┐
│ symbol │ price  │
├────────┼────────┤
│ AAPL   │ 150.25 │
│ MSFT   │ 300.50 │
└────────┴────────┘
```

Takes a byte array (U8 vector) that was previously serialized with `ser` and returns the reconstructed object.

!!! warning "Data Validity"
    The serialized data must be valid and complete. Invalid or corrupted data will result in an error. Version mismatches between serialization and deserialization may prevent successful reconstruction.

!!! note ""
    Deserialization preserves original types and structures. Complex objects like tables and dictionaries are fully reconstructed with their original data.
