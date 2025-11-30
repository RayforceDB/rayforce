# Parallel Map `pmap`

Applies a function to each element of a list **in parallel** using the thread pool and returns a new list with the results.

## Syntax

```clj
(pmap function list)
```

## Examples

### Basic parallel mapping

```clj
↪ (pmap (fn [x] (+ x 1)) [1 2 3])
[2 3 4]

↪ (pmap (fn [x] (* x 2)) [1 2 3])
[2 4 6]
```

### Performance comparison with map

```clj
;; Heavy sequential work benefits from parallelization
↪ (set work (fn [x] (fold + 0 (til 10000))))

↪ (timeit (map work (til 1000)))
105.99

↪ (timeit (pmap work (til 1000)))
7.05
```

## Notes

- The function is applied to each element **in parallel** using multiple threads
- Works with any type of list or vector
- Returns a new list without modifying the original
- Best for CPU-intensive tasks where parallel overhead is justified
- For small/fast tasks, sequential [`map`](map.md) may be faster due to lower overhead
- Thread pool size is determined by system CPU count
