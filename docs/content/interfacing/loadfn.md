# Load a function from the shared library `loadfn`

Somethimes it is necessary to create a plugin when current functionality is not enough. In this case, you can use the `loadfn` function to load a function from a shared library.

First, you need to create a shared library with the function you want to use. For example, let's create a shared library with a function that adds two numbers:

```c

#include <stdio.h>
#include "../core/rayforce.h"

obj_p myfn(obj_p a, obj_p b) {
    if (is_null(a) || is_null(b))
        return null(0);

    if (a->type != -TYPE_I64 || b->type != -TYPE_I64)
        return error(ERR_TYPE, "Expected two i64 arguments, found: '%s, '%s", type_name(a->type), type_name(b->type));

    return i64(a->i64 + b->i64);
}
```

Then, let's create the Makefile with the following content:

```bash
CC = clang
TARGET = libexample.so
CFLAGS = -fPIC -Wall -Wextra -std=c17 -Ofast -march=native -fassociative-math -ftree-vectorize\
 -fno-math-errno -funsafe-math-optimizations -ffinite-math-only -funroll-loops -m64

default:
	$(CC) $(CFLAGS) -shared -fPIC -I../core -o $(TARGET) example.c

clean:
	rm -f $(TARGET)
```

After that, you can compile the shared library with the following command:

```bash
make
```

Now, you can use the `loadfn` function to load the function from the shared library:

```clj
↪ (set f (loadfn "ext/libexample.so" "myfn" 2))
@fn
↪ (f 1 2)
3
↪ (f 1 0Ni)
•• [E005] error: type mismatch
╭─[0]─┬ repl:1..1 in function: @anonymous
│ 1   │ (f 1 0Ni)
│     ┴ ‾‾‾‾‾‾‾‾‾ Expected two i64 arguments, found: 'i64, 'i32
```