---
title: "Benchmark Toolkit"
description: "Guide to using the Rayforce benchmark toolkit"
weight: 20
---

# Benchmark Toolkit

The Rayforce benchmark toolkit allows you to measure and compare the performance of Rayforce scripts. This guide explains how to use the toolkit effectively.

## Running Benchmarks

To run benchmarks, use the `make bench` command:

```bash
# Run all benchmarks
make bench

# Run a specific benchmark
make bench BENCH=string_ops
```

The `BENCH` environment variable specifies which benchmark to run. If not provided, all benchmarks in the `bench/scripts` directory will be executed.

## Benchmark Script Format

Benchmark scripts are Rayforce scripts with special comments that specify benchmark parameters:

```rayforce
;; --iterations=1000 --expected-time=10.0

;; Create a large list and perform operations on it
(let [n 10000
      lst (range n)]
  (-> lst
      (map (fn [x] (* x x)))
      (filter (fn [x] (> x (/ n 2))))
      (reduce +)
      (sort >)))
```

### Parameters

- `--iterations=N`: Number of times to run the benchmark (default: 1000)
- `--expected-time=N`: Expected execution time in milliseconds (default: 0.0)

## Results

Benchmark results are stored in `bench/results.json` and include:

- Minimum execution time
- Maximum execution time
- Average execution time
- Expected time
- System information
- Git commit hash
- Timestamp

### Example Output

```
Benchmark Results for string_ops:
----------------------------------------
Previous Run: 2025-03-27 15:03:45
Current Run:  2025-03-27 15:05:10

Performance Metrics:
  Min Time: 0.003 ms +200.0%
  Max Time: 0.041 ms +141.2%
  Avg Time: 0.005 ms +138.9%
  Exp Time: 10.000 ms -100.0%

Summary: Performance has degraded by 138.9% since last run
```

## Writing Benchmark Scripts

1. Create a new `.rf` file in the `bench/scripts` directory
2. Add benchmark parameters using the `;;` comment syntax
3. Write your benchmark code
4. Optionally, create an initialization script with the same name but `.init.rf` suffix

### Example

```rayforce
;; --iterations=1000 --expected-time=10.0

;; Create a large string and perform operations on it
(let [str "The quick brown fox jumps over the lazy dog. "
      n 1000]
  ;; String concatenation
  (let [long_str (reduce (range n) (fn [acc _] (str+ acc str)) "")
        ;; String splitting
        words (split long_str " ")
        ;; String joining
        joined (join words ",")
        ;; String replacement
        replaced (replace long_str "dog" "cat")]
    ;; Return results to prevent optimization
    [long_str words joined replaced]))
```

## Initialization Scripts

If you need to set up test data or perform initialization before running the benchmark, create an initialization script with the same name as your benchmark script but with a `.init.rf` suffix:

```rayforce
;; bench/scripts/example_bench.init.rf
;; This is an initialization script that runs before the main benchmark

;; Set random seed for reproducibility
(let [random_seed 42]
  (rand random_seed)
  ;; Create any necessary global variables or functions
  (def global_config {max_rows: 10000 
                     min_price: 0 
                     max_price: 1000}))
```

The initialization script runs once before the benchmark iterations begin.

## Best Practices

1. **Consistent Environment**: Run benchmarks in a consistent environment to get reliable results
2. **Warm-up**: Consider adding a warm-up iteration before timing
3. **Multiple Runs**: Run benchmarks multiple times to account for system variations
4. **Meaningful Tests**: Write benchmarks that test real-world scenarios
5. **Documentation**: Document what each benchmark is testing and why

## Troubleshooting

If you encounter issues:

1. Check that your benchmark script exists in `bench/scripts`
2. Verify the script syntax is correct
3. Ensure you have write permissions for `bench/results.json`
4. Check the system information in the results to verify the environment

## See Also

- [Benchmark Results](bench.md) - Overview of Rayforce's benchmark results 