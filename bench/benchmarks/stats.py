"""
Statistics computation for benchmark results.

Computes percentiles, throughput, and aggregated metrics.
Does NOT collapse results into single "best" numbers.
"""

from dataclasses import dataclass, field
from typing import Any

import pandas as pd

from .runner import BenchmarkResults, TaskResult


@dataclass
class TaskStatistics:
    """Computed statistics for a single task."""
    task_id: str
    adapter_name: str
    
    # Sample size
    n_samples: int = 0
    n_errors: int = 0
    
    # Timing statistics (all in milliseconds)
    min_ms: float = 0.0
    max_ms: float = 0.0
    mean_ms: float = 0.0
    median_ms: float = 0.0  # p50
    p95_ms: float = 0.0
    p99_ms: float = 0.0
    std_ms: float = 0.0
    
    # Throughput (when row count is available)
    rows_per_sec: float | None = None
    
    # Validation
    validation_passed: bool = True
    validation_error: str | None = None
    
    # Execution metadata
    cache_mode: str = "warm"
    warmup_iterations: int = 0
    measured_iterations: int = 0


@dataclass
class BenchmarkStatistics:
    """Aggregated statistics for a complete benchmark run."""
    suite_name: str
    dataset_name: str
    
    # Per-task statistics
    task_stats: list[TaskStatistics] = field(default_factory=list)
    
    # Summary DataFrame for easy comparison
    summary_df: pd.DataFrame | None = None
    
    # Pivot table: tasks × adapters with median times
    comparison_df: pd.DataFrame | None = None


def compute_statistics(results: BenchmarkResults) -> BenchmarkStatistics:
    """Compute statistics from raw benchmark results.
    
    Args:
        results: Raw results from BenchmarkRunner.run()
    
    Returns:
        BenchmarkStatistics with computed metrics and DataFrames.
    """
    stats = BenchmarkStatistics(
        suite_name=results.suite_name,
        dataset_name=results.dataset_name,
    )
    
    summary_rows = []
    
    for task_result in results.task_results:
        task_stats = _compute_task_statistics(task_result)
        stats.task_stats.append(task_stats)
        
        summary_rows.append({
            "task": task_stats.task_id,
            "adapter": task_stats.adapter_name,
            "n": task_stats.n_samples,
            "errors": task_stats.n_errors,
            "min_ms": task_stats.min_ms,
            "median_ms": task_stats.median_ms,
            "p95_ms": task_stats.p95_ms,
            "max_ms": task_stats.max_ms,
            "std_ms": task_stats.std_ms,
            "rows/s": task_stats.rows_per_sec,
            "cache": task_stats.cache_mode,
            "valid": task_stats.validation_passed,
        })
    
    if summary_rows:
        stats.summary_df = pd.DataFrame(summary_rows)
        
        # Create comparison pivot table (tasks × adapters)
        if len(stats.summary_df) > 0:
            stats.comparison_df = stats.summary_df.pivot_table(
                index="task",
                columns="adapter",
                values="median_ms",
                aggfunc="first"
            )
    
    return stats


def _compute_task_statistics(task_result: TaskResult) -> TaskStatistics:
    """Compute statistics for a single task result."""
    stats = TaskStatistics(
        task_id=task_result.task_id,
        adapter_name=task_result.adapter_name,
        cache_mode=task_result.cache_mode,
        warmup_iterations=task_result.warmup_iterations,
        measured_iterations=task_result.measured_iterations,
        n_errors=len(task_result.errors),
        validation_passed=task_result.validation_passed,
        validation_error=task_result.validation_error,
    )
    
    if not task_result.timings_ns:
        return stats
    
    # Convert to milliseconds for statistics
    times_ms = [t / 1_000_000 for t in task_result.timings_ns]
    n = len(times_ms)
    stats.n_samples = n
    
    # Basic stats
    times_sorted = sorted(times_ms)
    stats.min_ms = times_sorted[0]
    stats.max_ms = times_sorted[-1]
    stats.mean_ms = sum(times_ms) / n
    stats.median_ms = _percentile(times_sorted, 0.50)
    stats.p95_ms = _percentile(times_sorted, 0.95)
    stats.p99_ms = _percentile(times_sorted, 0.99)
    
    # Standard deviation
    if n > 1:
        variance = sum((t - stats.mean_ms) ** 2 for t in times_ms) / (n - 1)
        stats.std_ms = variance ** 0.5
    
    # Throughput (rows per second)
    if task_result.row_counts:
        row_count = task_result.row_counts[0]  # Assume all iterations return same count
        if stats.median_ms > 0:
            stats.rows_per_sec = row_count / (stats.median_ms / 1000)
    
    return stats


def _percentile(sorted_values: list[float], p: float) -> float:
    """Compute percentile from sorted values using linear interpolation."""
    n = len(sorted_values)
    if n == 0:
        return 0.0
    if n == 1:
        return sorted_values[0]
    
    k = (n - 1) * p
    f = int(k)
    c = f + 1 if f + 1 < n else f
    d = k - f
    
    return sorted_values[f] * (1 - d) + sorted_values[c] * d


def format_comparison_table(stats: BenchmarkStatistics) -> str:
    """Format comparison DataFrame as a readable string table."""
    if stats.comparison_df is None:
        return "No comparison data available"
    
    # Format numbers with appropriate precision
    formatted = stats.comparison_df.copy()
    for col in formatted.columns:
        formatted[col] = formatted[col].apply(
            lambda x: f"{x:.2f}" if pd.notna(x) else "N/A"
        )
    
    return formatted.to_string()


def results_to_json(results: BenchmarkResults) -> dict[str, Any]:
    """Convert benchmark results to JSON-serializable dictionary."""
    return {
        "suite_name": results.suite_name,
        "dataset_name": results.dataset_name,
        "started_at": results.started_at,
        "finished_at": results.finished_at,
        "system_info": results.system_info,
        "adapter_info": results.adapter_info,
        "task_results": [
            {
                "task_id": tr.task_id,
                "adapter_name": tr.adapter_name,
                "cache_mode": tr.cache_mode,
                "warmup_iterations": tr.warmup_iterations,
                "measured_iterations": tr.measured_iterations,
                "timings_ns": tr.timings_ns,
                "row_counts": tr.row_counts,
                "checksums": tr.checksums,
                "validation_passed": tr.validation_passed,
                "validation_error": tr.validation_error,
                "errors": tr.errors,
            }
            for tr in results.task_results
        ],
    }
