# Serialization

Serialization is the process of converting nearly any Rayforce datatypes into a format suitable for storage or transmission, say: vector of bytes. This fundamental concept is integral to many software operations, from persisting object states to facilitating remote procedure calls.  

Let's dive in.

Each serialized object starts from the 16-bytes header:

``` c
typedef struct header_t
{
    u32_t prefix;
    u8_t  version;
    u8_t  flags;
    u16_t reserved;
    u64_t size;
} header_t;
```

Next, there is a type of the object. It's a single byte. Then, depending of the object type, there is a payload. For example, if the object is a vector, then there is u64_t length and a vector of atoms. If the object is a Dict or Table then there is a pair of keys and values. If the object is a function, then there is a string - source code of the function.

With this foundation, let's delve deeper into the world of serialization in Rayforce.
