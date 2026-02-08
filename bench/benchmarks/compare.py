"""
Baseline comparison for benchmark results.

Compares current run against previous results stored in docs/data.json
to show improvements/degradations.
"""

import json
from pathlib import Path
from typing import Any


# ANSI color codes for terminal output
class Colors:
    GREEN = "\033[92m"
    RED = "\033[91m"
    YELLOW = "\033[93m"
    CYAN = "\033[96m"
    BOLD = "\033[1m"
    DIM = "\033[2m"
    RESET = "\033[0m"


def load_baseline(baseline_path: Path) -> dict[str, Any] | None:
    """Load baseline results from data.json.
    
    Args:
        baseline_path: Path to data.json file
        
    Returns:
        Parsed baseline data or None if not found
    """
    if not baseline_path.exists():
        return None
    
    try:
        with open(baseline_path) as f:
            return json.load(f)
    except (json.JSONDecodeError, IOError):
        return None


def extract_baseline_times(baseline: dict[str, Any]) -> dict[str, dict[str, float]]:
    """Extract median times per adapter/task from baseline.
    
    Returns:
        {adapter: {task: median_ms}}
    """
    import statistics
    
    times = {}
    
    # Handle task_results format (from generate_report)
    task_results = baseline.get("task_results", [])
    for result in task_results:
        adapter = result.get("adapter_name")
        task = result.get("task_id")
        timings_ns = result.get("timings_ns", [])
        
        if adapter and task and timings_ns:
            if adapter not in times:
                times[adapter] = {}
            # Calculate median in milliseconds
            median_ns = statistics.median(timings_ns)
            times[adapter][task] = median_ns / 1_000_000
    
    # Also handle "results" format (alternative structure)
    results = baseline.get("results", [])
    for result in results:
        adapter = result.get("adapter")
        task = result.get("task")
        stats = result.get("stats", {})
        median = stats.get("median_ms")
        
        if adapter and task and median is not None:
            if adapter not in times:
                times[adapter] = {}
            times[adapter][task] = median
    
    return times


def _extract_current_times(current_stats) -> dict[str, dict[str, float]]:
    """Extract median times from BenchmarkStatistics object.
    
    Returns:
        {adapter: {task: median_ms}}
    """
    times = {}
    
    # Handle BenchmarkStatistics object
    if hasattr(current_stats, 'task_stats'):
        for task_stat in current_stats.task_stats:
            adapter = task_stat.adapter_name
            task = task_stat.task_id
            median = task_stat.median_ms
            
            if adapter not in times:
                times[adapter] = {}
            times[adapter][task] = median
    
    # Handle dict format
    elif isinstance(current_stats, dict):
        for adapter, tasks in current_stats.items():
            if adapter not in times:
                times[adapter] = {}
            for task, stats in tasks.items():
                if isinstance(stats, dict):
                    times[adapter][task] = stats.get("median_ms", 0)
                else:
                    times[adapter][task] = stats
    
    return times


def compare_results(
    current_stats,
    baseline_path: Path,
    focus_adapter: str | None = None,
    threshold_pct: float = 5.0,
    current_dataset: str | None = None,
    current_suite: str | None = None,
) -> str:
    """Compare current results against baseline and format diff.
    
    Args:
        current_stats: Current run statistics (BenchmarkStatistics or dict)
        baseline_path: Path to baseline data.json
        focus_adapter: Adapter to highlight (default: all)
        threshold_pct: Threshold for highlighting changes (default: 5%)
        current_dataset: Current dataset name (for validation)
        current_suite: Current suite name (for validation)
        
    Returns:
        Formatted comparison string for stdout
    """
    baseline = load_baseline(baseline_path)
    
    if baseline is None:
        return f"{Colors.DIM}No baseline found at {baseline_path}. This run will become the baseline.{Colors.RESET}"
    
    # Check if baseline matches current dataset/suite
    baseline_dataset = baseline.get("dataset_name")
    baseline_suite = baseline.get("suite_name")
    
    if current_dataset and baseline_dataset and current_dataset != baseline_dataset:
        return (
            f"{Colors.DIM}Baseline dataset mismatch: current={current_dataset}, "
            f"baseline={baseline_dataset}. Skipping comparison.{Colors.RESET}"
        )
    
    baseline_times = extract_baseline_times(baseline)
    current_times = _extract_current_times(current_stats)
    
    if not baseline_times:
        return f"{Colors.DIM}Baseline has no valid results to compare.{Colors.RESET}"
    
    if not current_times:
        return f"{Colors.DIM}No current results to compare.{Colors.RESET}"
    
    # Build comparison data
    lines = []
    lines.append("")
    lines.append(f"{Colors.BOLD}{'='*60}{Colors.RESET}")
    lines.append(f"{Colors.BOLD}Comparison vs Baseline{Colors.RESET}")
    lines.append(f"{Colors.BOLD}{'='*60}{Colors.RESET}")
    lines.append("")
    
    # Determine which adapters to show
    adapters_to_show = [focus_adapter] if focus_adapter else list(current_times.keys())
    
    improvements = 0
    degradations = 0
    unchanged = 0
    
    for adapter in adapters_to_show:
        if adapter not in current_times:
            continue
            
        adapter_baseline = baseline_times.get(adapter, {})
        if not adapter_baseline:
            lines.append(f"{Colors.DIM}{adapter}: No baseline data{Colors.RESET}")
            continue
        
        adapter_lines = []
        adapter_improvements = 0
        adapter_degradations = 0
        
        for task, current_median in current_times[adapter].items():
            baseline_median = adapter_baseline.get(task)
            
            if baseline_median is None or baseline_median == 0:
                continue
            
            # Calculate change
            change_pct = ((current_median - baseline_median) / baseline_median) * 100
            abs_change = abs(change_pct)
            
            # Format the line
            if abs_change < threshold_pct:
                # Within threshold - unchanged
                symbol = "="
                color = Colors.DIM
                unchanged += 1
            elif change_pct < 0:
                # Improvement (faster)
                symbol = "↑"
                color = Colors.GREEN
                improvements += 1
                adapter_improvements += 1
            else:
                # Degradation (slower)
                symbol = "↓"
                color = Colors.RED
                degradations += 1
                adapter_degradations += 1
            
            # Format: task: current_ms (baseline_ms) [+/-X%]
            change_str = f"{change_pct:+.1f}%"
            line = (
                f"  {color}{symbol} {task:20} "
                f"{current_median:8.2f} ms "
                f"{Colors.DIM}(was {baseline_median:.2f} ms){Colors.RESET} "
                f"{color}[{change_str}]{Colors.RESET}"
            )
            adapter_lines.append((abs_change, line))
        
        if adapter_lines:
            # Sort by absolute change (biggest changes first)
            adapter_lines.sort(key=lambda x: -x[0])
            
            # Adapter header with summary
            summary_parts = []
            if adapter_improvements > 0:
                summary_parts.append(f"{Colors.GREEN}↑{adapter_improvements}{Colors.RESET}")
            if adapter_degradations > 0:
                summary_parts.append(f"{Colors.RED}↓{adapter_degradations}{Colors.RESET}")
            summary = f" ({', '.join(summary_parts)})" if summary_parts else ""
            
            lines.append(f"{Colors.CYAN}{Colors.BOLD}{adapter}{Colors.RESET}{summary}")
            for _, line in adapter_lines:
                lines.append(line)
            lines.append("")
    
    # Summary
    lines.append(f"{Colors.BOLD}Summary:{Colors.RESET}")
    if improvements > 0:
        lines.append(f"  {Colors.GREEN}↑ {improvements} improved (faster){Colors.RESET}")
    if degradations > 0:
        lines.append(f"  {Colors.RED}↓ {degradations} degraded (slower){Colors.RESET}")
    if unchanged > 0:
        lines.append(f"  {Colors.DIM}= {unchanged} unchanged (within {threshold_pct}%){Colors.RESET}")
    
    if degradations > 0:
        lines.append("")
        lines.append(f"{Colors.YELLOW}⚠ Performance regressions detected!{Colors.RESET}")
    elif improvements > 0:
        lines.append("")
        lines.append(f"{Colors.GREEN}✓ Performance improved!{Colors.RESET}")
    
    return "\n".join(lines)


def format_quick_summary(
    current_stats,
    baseline_path: Path,
    focus_adapter: str = "rayforce",
) -> str:
    """Format a quick one-line summary for CI output.
    
    Args:
        current_stats: Current run statistics (BenchmarkStatistics or dict)
        baseline_path: Path to baseline data.json
        focus_adapter: Adapter to focus on
        
    Returns:
        One-line summary string
    """
    baseline = load_baseline(baseline_path)
    
    if baseline is None:
        return "No baseline - first run"
    
    baseline_times = extract_baseline_times(baseline)
    current_times = _extract_current_times(current_stats)
    adapter_baseline = baseline_times.get(focus_adapter, {})
    adapter_current = current_times.get(focus_adapter, {})
    
    if not adapter_baseline or not adapter_current:
        return f"No baseline data for {focus_adapter}"
    
    total_current = 0
    total_baseline = 0
    count = 0
    
    for task, current_median in adapter_current.items():
        baseline_median = adapter_baseline.get(task)
        
        if baseline_median is not None:
            total_current += current_median
            total_baseline += baseline_median
            count += 1
    
    if count == 0 or total_baseline == 0:
        return "No comparable tasks"
    
    overall_change = ((total_current - total_baseline) / total_baseline) * 100
    
    if overall_change < -5:
        return f"{Colors.GREEN}↑ {focus_adapter}: {overall_change:+.1f}% faster overall{Colors.RESET}"
    elif overall_change > 5:
        return f"{Colors.RED}↓ {focus_adapter}: {overall_change:+.1f}% slower overall{Colors.RESET}"
    else:
        return f"{Colors.DIM}= {focus_adapter}: {overall_change:+.1f}% (within margin){Colors.RESET}"
