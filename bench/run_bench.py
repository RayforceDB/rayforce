#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Rayforce Benchmark CLI

Usage:
    python run_bench.py --suite suites/groupby.yaml --dataset datasets/h2oai_groupby_1e7
    python run_bench.py --suite suites/groupby.yaml --adapters duckdb
    python run_bench.py --list-adapters
"""

import argparse
import sys
from pathlib import Path

# Add project root to path for imports
PROJECT_ROOT = Path(__file__).parent
sys.path.insert(0, str(PROJECT_ROOT))

from benchmarks import BenchmarkRunner, generate_report
from benchmarks.stats import compute_statistics, format_comparison_table
from benchmarks.compare import compare_results, format_quick_summary


def get_available_adapters() -> dict[str, type]:
    """Get all available adapter classes."""
    adapters = {}
    
    try:
        from adapters.duckdb_adapter import DuckDBAdapter
        adapters["duckdb"] = DuckDBAdapter
    except ImportError as e:
        print(f"Warning: DuckDB adapter not available: {e}")
    
    try:
        from adapters.rayforce_adapter import RayforceAdapter
        adapters["rayforce"] = RayforceAdapter
    except ImportError as e:
        print(f"Warning: Rayforce adapter not available: {e}")
    
    try:
        from adapters.kdb_adapter import KDBAdapter
        adapters["kdb"] = KDBAdapter
    except ImportError as e:
        print(f"Warning: KDB adapter not available: {e}")
    
    try:
        from adapters.polars_adapter import PolarsAdapter
        adapters["polars"] = PolarsAdapter
    except ImportError as e:
        print(f"Warning: Polars adapter not available: {e}")

    try:
        from adapters.pandas_adapter import PandasAdapter
        adapters["pandas"] = PandasAdapter
    except ImportError as e:
        print(f"Warning: Pandas adapter not available: {e}")

    try:
        from adapters.questdb_adapter import QuestDBAdapter
        adapters["questdb"] = QuestDBAdapter
    except ImportError as e:
        print(f"Warning: QuestDB adapter not available: {e}")

    return adapters


def list_adapters() -> None:
    """List available adapters and their status."""
    adapters = get_available_adapters()
    
    print("\nAvailable Adapters:")
    print("-" * 60)
    
    if not adapters:
        print("  No adapters available. Install required dependencies.")
        return
    
    for name, cls in adapters.items():
        try:
            instance = cls()
            print(f"  {name:15} v{instance.version:10} {'embedded' if instance.embedded else 'client/server'}")
        except Exception as e:
            print(f"  {name:15} (error: {e})")
    
    print()


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Rayforce Benchmark Framework",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Run group-by benchmark with DuckDB
  python run_bench.py --suite suites/groupby.yaml --dataset datasets/example_groupby --adapters duckdb

  # Run with all adapters
  python run_bench.py --suite suites/groupby.yaml --dataset datasets/h2oai_groupby_1e7

  # List available adapters
  python run_bench.py --list-adapters
        """
    )
    
    parser.add_argument(
        "--suite", "-s",
        type=Path,
        default=Path("suites/groupby.yaml"),
        help="Path to benchmark suite YAML file (default: suites/groupby.yaml)"
    )
    parser.add_argument(
        "--dataset", "-d",
        type=Path,
        default=Path("datasets/G1_1e7_1e2_0_0"),
        help="Path to dataset directory (default: datasets/G1_1e7_1e2_0_0)"
    )
    parser.add_argument(
        "--adapters", "-a",
        nargs="+",
        default=None,
        help="Adapters to run (default: all available)"
    )
    parser.add_argument(
        "--output", "-o",
        type=Path,
        default=Path("docs"),
        help="Output directory for reports (default: docs/ for GitHub Pages)"
    )
    parser.add_argument(
        "--list-adapters",
        action="store_true",
        help="List available adapters and exit"
    )
    parser.add_argument(
        "--quiet", "-q",
        action="store_true",
        help="Suppress progress output"
    )
    parser.add_argument(
        "--no-report",
        action="store_true",
        help="Skip HTML report generation"
    )
    parser.add_argument(
        "--duckdb-memory",
        type=str,
        default=None,
        help="DuckDB memory limit (e.g., '4GB')"
    )
    parser.add_argument(
        "--rayforce-binary",
        type=Path,
        default=None,
        help="Path to rayforce binary (overrides config)"
    )
    parser.add_argument(
        "--kdb-binary",
        type=Path,
        default=None,
        help="Path to kdb q binary (overrides config)"
    )
    parser.add_argument(
        "--questdb-host",
        type=str,
        default=None,
        help="QuestDB host (default: localhost)"
    )
    parser.add_argument(
        "--questdb-port",
        type=int,
        default=None,
        help="QuestDB PostgreSQL wire port (default: 8812)"
    )
    parser.add_argument(
        "--config",
        type=Path,
        default=None,
        help="Path to config directory (default: project root)"
    )
    parser.add_argument(
        "--compare",
        action="store_true",
        default=True,
        help="Compare results against baseline (default: enabled)"
    )
    parser.add_argument(
        "--no-compare",
        action="store_true",
        help="Disable comparison against baseline"
    )
    parser.add_argument(
        "--baseline",
        type=Path,
        default=None,
        help="Path to baseline data.json (default: <output>/data.json)"
    )
    parser.add_argument(
        "--focus-adapter",
        type=str,
        default=None,
        help="Focus comparison on specific adapter (default: show all)"
    )
    parser.add_argument(
        "--threshold",
        type=float,
        default=5.0,
        help="Threshold %% for highlighting changes (default: 5.0)"
    )
    parser.add_argument(
        "--continue-on-error",
        action="store_true",
        help="Continue running even if a task fails (default: stop on first error)"
    )
    
    args = parser.parse_args()
    
    # Load config if specified
    if args.config:
        from benchmarks.config import reload_config
        reload_config(args.config)
    
    # Handle --list-adapters
    if args.list_adapters:
        list_adapters()
        return 0
    
    # Validate paths exist
    if not args.suite.exists():
        print(f"Error: Suite file not found: {args.suite}")
        return 1
    
    if not args.dataset.exists():
        print(f"Error: Dataset directory not found: {args.dataset}")
        return 1
    
    # Get available adapters
    available = get_available_adapters()
    
    if not available:
        print("Error: No adapters available. Install required dependencies.")
        return 1
    
    # Create runner
    runner = BenchmarkRunner(
        verbose=not args.quiet,
        stop_on_error=not args.continue_on_error,
    )
    
    # Register adapters
    if args.adapters:
        adapter_names = args.adapters
    else:
        adapter_names = list(available.keys())
    
    for name in adapter_names:
        if name not in available:
            print(f"Warning: Adapter '{name}' not available, skipping")
            continue
        
        # Create adapter with CLI overrides (config is used as default)
        if name == "duckdb":
            adapter = available[name](
                memory_limit=args.duckdb_memory,
            )
        elif name == "rayforce":
            adapter = available[name](
                binary_path=args.rayforce_binary,
            )
        elif name == "kdb":
            adapter = available[name](
                binary_path=args.kdb_binary,
            )
        elif name == "questdb":
            adapter = available[name](
                host=args.questdb_host,
                port=args.questdb_port,
            )
        else:
            adapter = available[name]()
        
        runner.register_adapter(adapter)
    
    # Run benchmark
    print(f"\n{'='*60}")
    print(f"Running benchmark suite: {args.suite.stem}")
    print(f"Dataset: {args.dataset.name}")
    print(f"Adapters: {', '.join(adapter_names)}")
    print(f"{'='*60}\n")
    
    try:
        results = runner.run(
            suite_path=args.suite,
            dataset_dir=args.dataset,
            adapter_names=adapter_names if args.adapters else None,
        )
    except Exception as e:
        print(f"\nError during benchmark execution: {e}")
        return 1
    
    # Compute and display statistics
    stats = compute_statistics(results)
    
    print(f"\n{'='*60}")
    print("Results Summary")
    print(f"{'='*60}\n")
    print(format_comparison_table(stats))
    
    # Compare against baseline (before generating new report)
    if not args.no_compare:
        baseline_path = args.baseline or (args.output / "data.json")
        comparison = compare_results(
            stats,
            baseline_path,
            focus_adapter=args.focus_adapter,
            threshold_pct=args.threshold,
            current_dataset=args.dataset.name,
            current_suite=args.suite.stem,
        )
        print(comparison)
    
    # Generate report (this overwrites the baseline)
    if not args.no_report:
        try:
            report_path = generate_report(results, args.output)
            print(f"\n✓ Report generated: {report_path}")
        except Exception as e:
            print(f"\nWarning: Failed to generate report: {e}")
    
    # Check for failures
    failed_tasks = [
        tr for tr in results.task_results 
        if not tr.success or not tr.validation_passed
    ]
    
    if failed_tasks:
        print(f"\n⚠ {len(failed_tasks)} task(s) failed or failed validation")
        return 1
    
    print(f"\n✓ All tasks completed successfully")
    return 0


if __name__ == "__main__":
    sys.exit(main())
