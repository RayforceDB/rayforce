# :material-package: Serialization

Serialization is the process of converting nearly any Rayforce datatypes into a format suitable for storage or transmission, say: vector of bytes. This fundamental concept is integral to many software operations, from persisting object states to facilitating remote procedure calls.  

## Binary Format Structure

### Header (16 bytes)
Each serialized object starts with a 16-byte header:

```c
typedef struct ipc_header_t
{
    u32_t prefix;     // Magic number (0xRAF0) to identify Rayforce serialized data
    u8_t  version;    // Serialization format version (current: 1)
    u8_t  flags;      // Additional flags for special handling (compression, encryption)
    u16_t reserved;   // Reserved for future use (must be 0)
    i64_t size;       // Total size of the payload in bytes
} ipc_header_t;
```

### Type Identifier (1 byte)
Next, there is a type of the object. It's a single byte that identifies the data type:

```clj
1: Integer (I64)      // 64-bit signed integer
2: Array              // Homogeneous array of values
3: String            // UTF-8 encoded string
4: Symbol            // Interned symbol
5: Function          // Function definition
6: Dictionary        // Key-value pairs
7: Table            // Columnar data structure
8: Float (F64)      // 64-bit floating point
9: List             // Heterogeneous list
// ... etc
```

### Payload Format
The payload format depends on the object type:

#### Simple Types
- Integer (I64): 8 bytes, little-endian
- Float (F64): 8 bytes, IEEE 754
- Symbol: UTF-8 bytes of symbol name

#### Compound Types
- Vectors: 
  - u64: length
  - [type byte]
  - [elements...]

- Strings: 
  - u64: length in bytes
  - [UTF-8 encoded bytes]

- Dictionaries:
  - u64: number of pairs
  - [keys section]
  - [values section]

- Tables:
  - u64: number of columns
  - u64: number of rows
  - [column names]
  - [column data]

- Functions:
  - u64: source code length
  - [UTF-8 source code]
  - [captured environment]

!!! info
    The serialization format is designed to be:
    - Compact: minimal overhead for simple types
    - Self-describing: includes type information
    - Extensible: can handle new types
    - Version-aware: supports format evolution
    - Endian-aware: consistent byte ordering

!!! warning "Important Considerations"
    - When serializing functions, only the source code is preserved
    - The runtime environment must have all required dependencies
    - Large objects are not compressed by default
    - Version mismatches will prevent deserialization
    - Invalid data may cause deserialization errors

!!! tip "Best Practices"
    Use serialization when you need to:
    - Store data persistently
    - Transfer data between processes
    - Create checkpoints or backups
    - Implement caching mechanisms
    - Share data between different Rayforce instances

!!! note "Version Compatibility"
    - Version 1: Basic type support
    - Future versions may add:
        - Compression support
        - Encryption
        - Custom type handlers
        - Schema validation
