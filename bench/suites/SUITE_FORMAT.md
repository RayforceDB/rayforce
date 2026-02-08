# Benchmark Suite Format

Suites are defined in YAML format and describe a set of benchmark tasks to run.

## Schema

```yaml
name: suite_name
description: Human-readable description
version: "1.0.0"

# Dataset reference (directory name under datasets/)
dataset: dataset_name

# Default execution parameters
warmup: 3          # Warmup iterations before measurement
iterations: 10     # Measured iterations
cache_mode: warm   # "warm" (default) or "cold"

# Benchmark tasks
tasks:
  - id: task_identifier
    description: Task description
    params:
      key: value
    # Optional overrides
    warmup: 5
    iterations: 20
    cache_mode: cold
    expected_rows: 1000
    expected_checksum: 12345678

# Optional metadata
metadata:
  source: Origin of the benchmark
  url: Reference URL
```

## Fields

### Suite-Level

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `name` | string | Yes | Unique suite identifier |
| `description` | string | No | Human-readable description |
| `version` | string | No | Suite version |
| `dataset` | string | Yes | Dataset directory name |
| `warmup` | integer | No | Default warmup iterations (default: 3) |
| `iterations` | integer | No | Default measured iterations (default: 10) |
| `cache_mode` | string | No | "warm" or "cold" (default: warm) |
| `metadata` | object | No | Arbitrary metadata |

### Task-Level

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `id` | string | Yes | Task identifier (matches adapter task handlers) |
| `description` | string | No | Task description |
| `params` | object | No | Task-specific parameters |
| `warmup` | integer | No | Override suite-level warmup |
| `iterations` | integer | No | Override suite-level iterations |
| `cache_mode` | string | No | Override suite-level cache mode |
| `expected_rows` | integer | No | Expected row count for validation |
| `expected_checksum` | integer | No | Expected result checksum |

## Built-in Task IDs

### Group By Tasks (H2OAI compatible)

| Task ID | Description |
|---------|-------------|
| `groupby_q1` | sum(v1) by id1 |
| `groupby_q2` | sum(v1) by id1, id2 |
| `groupby_q3` | sum(v1), avg(v3) by id3 |
| `groupby_q4` | avg(v1), avg(v2), avg(v3) by id4 |
| `groupby_q5` | sum(v1), sum(v2), sum(v3) by id6 |
| `groupby_q6` | max(v1) - min(v2) by id3 |
| `groupby_q7` | sum(v3), count by id1-id6 |

### Join Tasks

| Task ID | Description |
|---------|-------------|
| `left_join` | Left join on keys |
| `inner_join` | Inner join on keys |

### Generic Tasks

| Task ID | Description |
|---------|-------------|
| `sql` | Execute arbitrary SQL (DuckDB) |
| `eval` | Execute arbitrary expression (Rayforce) |

## Cache Modes

### Warm Mode (default)

- Caches are populated before measured iterations
- Measures steady-state performance
- Most representative of typical usage

### Cold Mode

- Caches are cleared before each iteration
- `adapter.clear_cache()` is called before each run
- Measures startup/cold-path performance
- Useful for testing cache efficiency

## Validation

### Row Count Validation

If `expected_rows` is provided:
- All iterations must return the same row count
- Row count must match `expected_rows`
- Test fails if validation fails

### Checksum Validation

If `expected_checksum` is provided:
- Adapter computes checksum from results
- Checksum must match expected value
- Prevents "fast but wrong" results

## Example Suites

### Simple Group By

```yaml
name: simple_groupby
dataset: example_data
iterations: 5

tasks:
  - id: groupby_q1
  - id: groupby_q2
```

### Custom SQL

```yaml
name: custom_queries
dataset: my_data

tasks:
  - id: sql
    description: Custom aggregation
    params:
      query: "SELECT category, COUNT(*) FROM t GROUP BY category"
    expected_rows: 10

  - id: sql
    description: Complex filter
    params:
      query: "SELECT * FROM t WHERE value > 100 AND date > '2024-01-01'"
```

### Cold Cache Testing

```yaml
name: cold_cache_test
dataset: benchmark_data
cache_mode: cold
warmup: 0
iterations: 5

tasks:
  - id: groupby_q1
  - id: groupby_q7
```
