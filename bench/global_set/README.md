# Indexed global writes (#951)

The change indexes symbol-to-slot lookup for writes only. The existing env
lock, global value epoch, slot order, user flags, and read path are preserved.
The fixed 1,024-slot binding table uses a 2,048-bucket index of slot-plus-one
values (4 KiB), with no per-value allocation. New names and misses use the
same index. Deletion already shifts slots; it rebuilds the small index under
the lock. Runtime initialization/destruction clears it with the env.

## Reproduce

The issue's compiled Rayfall loops are in `global_set.rfl`:

```sh
make release -j4
./rayforce -c 1 -f bench/global_set/global_set.rfl
```

To isolate lookup/refcount cost from Rayfall dispatch and value allocation:

```sh
make lib -j4
cc -O3 -Iinclude -Isrc bench/global_set/global_set.c librayforce.a \
  -lm -lpthread -o /tmp/global-set
RAYFORCE_CORES=1 /tmp/global-set 1000000
```

The C program prebinds 512 user names, then measures same-symbol writes,
rotating-symbol writes, unchanged reads, and a dotted-write control. It uses
CPU time from C `clock()` and checks its read checksum. Run against both
baseline and candidate libraries on the same host.

## Local results

Measured on 2026-10-23 with optimized Clang builds, based on upstream
`d4899d91`. For the comparison, only the `env.c` object was replaced: all
other release objects and compiler flags were identical. The native program
reported 864 initial bindings. Five process runs per variant alternated
before/after order; the table gives medians in nanoseconds per operation.

| Native operation | Before | Indexed writes |
|---|---:|---:|
| Same symbol, near end of env | 219.61 | 20.57 |
| Rotate over 512 symbols | 170.54 | 18.27 |
| Read control | 1.59 | 1.60 |
| Dotted-write control | 283.38 | 65.11 |

The issue's one-million-iteration loops, medians of five samples in ms:

| Rayfall loop | Before | Indexed writes |
|---|---:|---:|
| Local counter | 61.00 | 61.06 |
| Global counter | 175.25 | 80.31 |
| Local counter + global write | 193.09 | 82.22 |

All loop checksums agree. These are observations, not hard performance tests.
Lock elision and per-slot value versions are separate changes and are not
needed for this lookup improvement.

Focused sanitizer coverage includes randomized delete/reinsert/rebind,
runtime recreation, full-capacity rebinding and slot reuse, builtin user
flags, cached values (including exact dotted builtin names), environment
lookup, snapshot/replay, and persisted-symbol runtime initialization.
