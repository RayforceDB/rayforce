# Memory Model

!!! note "No system allocator"
    Rayforce never calls `malloc`, `calloc`, `realloc`, or `free`. All allocation goes through `ray_alloc` / `ray_free` (general) or `ray_arena_alloc` (bulk short-lived). Only allocator internals may use `ray_sys_alloc` / `ray_sys_free`.

## The ray_t Block Header

Every object in Rayforce — atoms, vectors, lists, tables — begins with a 32-byte `ray_t` header. Data follows immediately at byte 32 via a flexible array member (`data[]`).

```c
typedef union ray_t {
    /* Allocated: object header */
    struct {
        /* Bytes 0-15: nullable bitmask / slice / ext nullmap */
        union {
            uint8_t  nullmap[16];
            struct { ray_t* slice_parent; int64_t slice_offset; };
            struct { ray_t* ext_nullmap;  ray_t* sym_dict; };
            struct { ray_t* str_ext_null; ray_t* str_pool; };
        };
        /* Bytes 16-31: metadata + value */
        uint8_t  mmod;       /* 0=heap, 1=file-mmap              */
        uint8_t  order;      /* block order (size = 2^order)     */
        int8_t   type;       /* negative=atom, positive=vector   */
        uint8_t  attrs;      /* attribute flags                  */
        uint32_t rc;         /* reference count (0=free)         */
        union {
            int64_t  i64;    /* I64/SYMBOL/DATE/TIME atom        */
            double   f64;    /* F64 atom                         */
            ray_t*   obj;    /* pointer to child                 */
            struct { uint8_t slen; char sdata[7]; }; /* SSO string */
            int64_t  len;    /* vector element count             */
        };
        uint8_t  data[];     /* element data (flexible array)    */
    };
    /* Free: buddy allocator block */
    struct {
        ray_t* fl_prev;
        ray_t* fl_next;
    };
} ray_t;
```

### Field Details

| Bytes | Field | Purpose |
|---|---|---|
| 0-15 | `aux[16]` | Type-specific auxiliary union. For `RAY_SYM` columns this can point to the symbol domain, for `RAY_STR` columns to the string pool, for slices to parent/offset metadata, and for allocator free blocks to free-list pointers. Vector null state lives in type-correct sentinel values plus the `RAY_ATTR_HAS_NULLS` hint, not in a separate bitmap. |
| 16 | `mmod` | Memory mode: 0 = heap-allocated, 1 = file memory-mapped |
| 17 | `order` | Buddy allocator block order. Block size = 2^order bytes. Range: 6..38 (64 bytes to 256 GB). |
| 18 | `type` | Signed byte: negative = atom, 0 = LIST, 1-13 = vector types (BOOL through STR), 98 = TABLE, 99 = DICT, 100-103 = function types, 127 = ERROR |
| 19 | `attrs` | Attribute flags: `RAY_ATTR_ARENA` (arena-allocated, retain/release are no-ops), slice flag, sorted flag |
| 20-23 | `rc` | Reference count. 0 = free block. Incremented by `ray_retain`, decremented by `ray_release`. |
| 24-31 | `i64 / f64 / len` | For atoms: the value itself (up to 8 bytes, or 7-byte SSO string). For vectors: element count. For lists/tables: child count. |
| 32+ | `data[]` | Element array. Layout depends on type: contiguous typed elements for vectors, `ray_t*` pointers for lists/tables. |

!!! note "Dual-use union"
    When a block is free, bytes 0-15 are reused as `fl_prev`/`fl_next` free-list pointers. The buddy allocator chains free blocks through these fields without any additional bookkeeping memory.

## Buddy Allocator

The primary allocator uses a classic buddy system with order-based free lists. Block sizes are powers of two, ranging from order 6 (64 bytes, enough for a `ray_t` header + 32 bytes of data) to order 38 (256 GB).

### Allocation

1. Compute the minimum order that fits the requested size + 32-byte header
2. Search the free list for that order
3. If empty, try the next larger order and split the block, placing the unused half on the lower-order free list
4. Repeat until a block is found or allocation fails

### Deallocation

1. Check if the buddy block (the other half of the parent) is also free
2. If so, remove the buddy from its free list and coalesce both halves into a single block of order+1
3. Repeat coalescing until the buddy is not free or maximum order is reached
4. Place the final block on the appropriate free list

Constants governing the allocator:

```c
#define RAY_ORDER_MIN       6   /* minimum block: 64 bytes  */
#define RAY_HEAP_MAX_ORDER  38  /* maximum pool: 256 GB     */
```

### File-Backed Pool Fallback

Pools are normally backed by anonymous `mmap` — fast, lazy-committed by the kernel, bounded by RAM + swap. When the OS refuses an anon allocation (typically because RAM + swap can't cover the requested chunk right now), the buddy allocator falls back to a **file-backed** mmap pointed at a tempfile in the heap's configured swap directory. Dirty pages flush to the backing file on page-out, so fresh in-memory allocations — including a single huge vector like `(til 10000000000)` — can grow beyond physical RAM as long as the swap filesystem has room.

The fallback is transparent to every `ray_alloc` caller. Pages never written stay as holes in the sparse backing file, so the over-allocate-and-trim alignment trick costs zero real disk space; only the pages you actually touch consume disk pages. On heap teardown the fd is closed and the tempfile is unlinked, so the swap directory doesn't accumulate orphans.

- **Configurable swap directory.** The directory is resolved as `RAY_HEAP_SWAP` → `TMPDIR` → `/tmp`. `/tmp` is the default rather than the working directory, which may be read-only or shared. Trailing slash is added automatically. The chosen directory must exist and be writable by the running process.
- **Tempfile naming.** Pools use `rayheap_<pid>_<heap_id>_<counter>.dat`, direct blocks `raydirect_<pid>_<heap_id>_<counter>.dat`. The pid keeps processes of one PID namespace apart, and the counter is a per-process atomic. Files are opened `O_EXCL`, so a name that already exists fails that allocation rather than sharing the file.
- **POSIX only today.** The fallback path uses `open` / `ftruncate` / `mmap MAP_SHARED` / `munmap` / `unlink`. Windows pools currently take only the anonymous `VirtualAlloc` path.
- **Distinct from block offloading.** Block offloading (see [Block Offloading](offloading.md)) streams pre-existing parted-table data through queries one segment at a time. The file-backed pool fallback handles fresh anonymous allocations that exceed RAM. Both let workloads exceed RAM, but at different layers and for different shapes of work.

## Slab Cache

For the most common allocation sizes, a slab cache provides O(1) allocation and deallocation by maintaining per-order stacks of pre-split blocks.

```c
#define RAY_SLAB_CACHE_SIZE  64   /* max blocks cached per order   */
#define RAY_SLAB_ORDERS      11   /* orders 6..16 (64B to 64KB)    */
```

The slab cache covers orders 6 through 16 (64 bytes to 64 KB), which account for the vast majority of allocations: scalar atoms, short vectors, list nodes, string pool chunks, and morsel-sized buffers. When a slab is empty, it refills from the buddy allocator. When full, freed blocks fall through to buddy deallocation.

## Thread-Local Heaps

Each thread has its own heap, identified by a `heap_id` (u16). Heap IDs are allocated from an atomic bitmap, ensuring no two threads share a heap.

### Why Per-Thread Heaps?

The buddy allocator's free lists are not thread-safe by design. Rather than adding locks (which would destroy performance for the most common allocation path), each thread operates on its own set of free lists. This eliminates all contention on the fast path.

### Cross-Heap Free

When thread A allocates a block and thread B needs to free it (common in parallel query execution), the free is deferred:

1. Thread B pushes the block onto a **lock-free LIFO** (compare-and-swap linked list) associated with thread A's heap
2. Thread A periodically calls `ray_heap_flush_foreign()` to drain this LIFO and coalesce the blocks into its own free lists

The handoff API:

```c
ray_heap_push_pending(block);    /* enqueue to owning heap's LIFO   */
ray_heap_drain_pending();         /* drain LIFO into local free lists */
ray_heap_flush_foreign();        /* reclaim all foreign-freed blocks */
```

This design means the common case (thread-local alloc/free) is completely lock-free, and the uncommon case (cross-thread free) uses a single atomic CAS per block.

## Arena Allocator

For bulk short-lived allocations (e.g., the symbol intern table, temporary parse trees), Rayforce provides an arena (bump) allocator.

```c
/* Create an arena */
ray_arena_t arena;
ray_arena_init(&arena);

/* Allocate from the arena (bump pointer, no individual free) */
void* ptr = ray_arena_alloc(&arena, size);

/* Reset (reuse memory without returning to OS) */
ray_arena_reset(&arena);

/* Destroy (return all memory) */
ray_arena_destroy(&arena);
```

Blocks allocated from an arena carry the `RAY_ATTR_ARENA` flag in their `attrs` field. This flag makes `ray_retain` and `ray_release` no-ops, since arena blocks are freed all at once when the arena is destroyed. This eliminates reference counting overhead for temporary objects.

### Use Cases

- **Symbol intern table** — String atoms are arena-allocated since they live for the duration of the process
- **Parse trees** — Rayfall parser allocates AST nodes from a per-parse arena, freed after compilation
- **Temporary buffers** — Intermediate hash tables, sort buffers, and other transient structures

## COW Ref Counting

Rayforce uses **copy-on-write (COW)** semantics to allow safe sharing of data between multiple consumers without copying.

### API

| Function | Behavior |
|---|---|
| `ray_retain(v)` | Increment reference count. No-op if `RAY_ATTR_ARENA` is set, or if `v` is a `RAY_ERROR` object. |
| `ray_release(v)` | Decrement reference count. If it reaches zero, free the block (and recursively release children for lists/tables). No-op for arena blocks, and for `RAY_ERROR` objects (see caveat below). |
| `ray_cow(v)` | If `rc > 1`, allocate a new block and copy the data, returning the new block (caller owns it). If `rc == 1`, return the same block (caller already has exclusive ownership). Returns `v` unchanged if it is a `RAY_ERROR` object. |

!!! warning "`ray_release` is a no-op on error objects"
    `ray_retain`, `ray_release`, and `ray_cow` all early-return when handed a `RAY_ERROR` object (`RAY_IS_ERR(v)` is true). This means `ray_release` will **not** reclaim an error — a caller that holds the sole reference and wants the block freed must call `ray_error_free(err)` instead. Releasing an error is silently ignored, so the block leaks until heap teardown.

### COW Cleanup Pattern

After `ray_cow` returns a new copy, all error paths must release it to prevent leaks. The standard pattern uses `goto fail`:

```c
ray_t* original = vec;
vec = ray_cow(vec);
if (RAY_IS_ERR(vec)) return vec;

/* ... mutate vec ... */
if (error_condition) goto fail;

return vec;

fail:
    if (vec != original) ray_release(vec);
    return RAY_ERR_PTR(err);
```

### How COW Enables Zero-Copy Operations

- **Slices** — `ray_vec_slice` creates a new header pointing into the parent's data, incrementing the parent's refcount. No data is copied.
- **Table projections** — Selecting a subset of columns creates new table headers that share the underlying column vectors via retain.
- **CSR sharing** — Multiple graph queries can share the same CSR vectors. Mutations (e.g., adding edges) trigger COW only on the modified vector.
- **DAG intermediates** — When a DAG node's output feeds multiple consumers, the vector is shared via retain rather than copied.

## Allocation API Summary

| Function | Purpose | Thread Safety |
|---|---|---|
| `ray_alloc(size)` | General allocation (buddy + slab) | Thread-local (no locks) |
| `ray_free(v)` | General deallocation (may defer cross-heap) | Lock-free CAS for cross-heap |
| `ray_retain(v)` | Increment ref count | Plain inc; atomic only when the thread-local `ray_rc_sync` flag is set |
| `ray_release(v)` | Decrement ref count, free at zero | Plain dec + free; atomic only when `ray_rc_sync` is set |
| `ray_cow(v)` | Copy-on-write (copy if shared) | Plain rc read; atomic load only when `ray_rc_sync` is set |
| `ray_arena_alloc(a, size)` | Bump allocation from arena | Not thread-safe (per-arena) |
| `ray_arena_reset(a)` | Reset arena (reuse memory) | Not thread-safe |
| `ray_arena_destroy(a)` | Free all arena memory | Not thread-safe |
| `ray_sys_alloc(size)` | System allocator (mmap/VirtualAlloc) | Thread-safe (OS-level) |
| `ray_sys_free(ptr, size)` | System deallocator (munmap/VirtualFree) | Thread-safe (OS-level) |

!!! note "Refcount atomicity is conditional"
    `ray_retain`/`ray_release`/`ray_cow` use plain non-atomic increments/decrements on the fast path. They upgrade to atomic operations only when the thread-local `ray_rc_sync` flag is set, which the pool sets while dispatching work across worker threads (parallel query execution). Single-threaded evaluation never pays the atomic cost.

## Memory Layout Visualization

A complete picture of how a vector looks in memory:

```text
/* ray_t* vec = ray_vec_new(RAY_I64, 4); with values [10, 20, 30, 40] */

Offset  Field          Value
------  -----          -----
 0      nullmap[16]    00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00
16      mmod           0 (heap)
17      order          7 (block = 128 bytes = 32 header + 96 data)
18      type           5 (RAY_I64)
19      attrs          0
20      rc             1 (one reference)
24      len            4 (4 elements)
32      data[0]        10 (int64_t)
40      data[1]        20
48      data[2]        30
56      data[3]        40
```

For a vector with nulls, the `nullmap` bits at offset 0 indicate which elements are null (bit 0 = element 0, etc.). Vectors with more than 128 elements use `ext_nullmap` to point to a separate bitmap vector.
