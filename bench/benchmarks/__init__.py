"""
Rayforce Benchmark Framework

A vendor-neutral benchmarking framework for comparing database performance.
"""

from .adapter import Adapter, AdapterResult
from .runner import BenchmarkRunner
from .stats import compute_statistics
from .report import generate_report
from .config import Config, get_config, reload_config
from .compare import compare_results, format_quick_summary, load_baseline

__version__ = "0.1.0"

__all__ = [
    "Adapter",
    "AdapterResult", 
    "BenchmarkRunner",
    "compute_statistics",
    "generate_report",
    "Config",
    "get_config",
    "reload_config",
    "compare_results",
    "format_quick_summary",
    "load_baseline",
]
