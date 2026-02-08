"""
Benchmark runner - orchestrates benchmark execution.

Handles:
- Loading suites and datasets
- Executing warmup and measured iterations
- Cold and warm cache modes
- Result collection and validation
"""

import json
import time
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Any

import yaml

from .adapter import Adapter, AdapterResult, SetupError, TaskError


@dataclass
class TaskResult:
    """Results for a single task across all iterations."""
    task_id: str
    adapter_name: str
    
    # Raw timing data (all measured iterations, in nanoseconds)
    timings_ns: list[int] = field(default_factory=list)
    
    # Validation
    row_counts: list[int] = field(default_factory=list)
    checksums: list[int | None] = field(default_factory=list)
    validation_passed: bool = True
    validation_error: str | None = None
    
    # Execution mode
    cache_mode: str = "warm"  # "cold" or "warm"
    warmup_iterations: int = 0
    measured_iterations: int = 0
    
    # Query that was executed
    query: str | None = None
    
    # Error tracking
    errors: list[str] = field(default_factory=list)
    
    @property
    def success(self) -> bool:
        return len(self.errors) == 0 and self.validation_passed


@dataclass
class BenchmarkResults:
    """Complete results from a benchmark run."""
    suite_name: str
    dataset_name: str
    
    # Results per (adapter, task) combination
    task_results: list[TaskResult] = field(default_factory=list)
    
    # Metadata for reproducibility
    started_at: str = ""
    finished_at: str = ""
    system_info: dict[str, Any] = field(default_factory=dict)
    adapter_info: dict[str, dict[str, Any]] = field(default_factory=dict)
    suite_config: dict[str, Any] = field(default_factory=dict)
    dataset_manifest: dict[str, Any] = field(default_factory=dict)


class BenchmarkRunner:
    """Executes benchmark suites against database adapters."""
    
    def __init__(self, verbose: bool = True, stop_on_error: bool = True):
        self.verbose = verbose
        self.stop_on_error = stop_on_error
        self._adapters: dict[str, Adapter] = {}
    
    def register_adapter(self, adapter: Adapter) -> None:
        """Register an adapter for benchmarking."""
        self._adapters[adapter.name] = adapter
    
    def load_suite(self, suite_path: Path) -> dict[str, Any]:
        """Load a benchmark suite from YAML."""
        with open(suite_path) as f:
            return yaml.safe_load(f)
    
    def load_manifest(self, dataset_dir: Path) -> dict[str, Any]:
        """Load dataset manifest."""
        manifest_path = dataset_dir / "manifest.json"
        if not manifest_path.exists():
            raise SetupError(f"Dataset manifest not found: {manifest_path}")
        with open(manifest_path) as f:
            return json.load(f)
    
    def run(
        self,
        suite_path: Path,
        dataset_dir: Path,
        adapter_names: list[str] | None = None,
    ) -> BenchmarkResults:
        """Run a benchmark suite.
        
        Args:
            suite_path: Path to suite YAML file.
            dataset_dir: Path to dataset directory (contains manifest.json).
            adapter_names: List of adapter names to run (None = all registered).
        
        Returns:
            BenchmarkResults with all timing data and metadata.
        """
        results = BenchmarkResults(
            suite_name=suite_path.stem,
            dataset_name=dataset_dir.name,
            started_at=datetime.now().isoformat(),
        )
        
        # Load suite and manifest
        suite = self.load_suite(suite_path)
        manifest = self.load_manifest(dataset_dir)
        results.suite_config = suite
        results.dataset_manifest = manifest
        
        # Collect system info
        results.system_info = self._get_system_info()
        
        # Determine which adapters to run
        if adapter_names is None:
            adapter_names = list(self._adapters.keys())
        
        adapters_to_run = [
            self._adapters[name] for name in adapter_names
            if name in self._adapters
        ]
        
        if not adapters_to_run:
            raise SetupError(f"No valid adapters found: {adapter_names}")
        
        # Build CSV paths
        csv_paths = [
            dataset_dir / fname
            for fname in manifest.get("files", [])
        ]
        
        # Run benchmark for each adapter
        for adapter in adapters_to_run:
            self._log(f"\n{'='*60}")
            self._log(f"Running adapter: {adapter.name}")
            self._log(f"{'='*60}")
            
            results.adapter_info[adapter.name] = adapter.get_info()
            
            try:
                self._run_adapter(
                    adapter=adapter,
                    suite=suite,
                    manifest=manifest,
                    csv_paths=csv_paths,
                    results=results,
                )
            except Exception as e:
                self._log(f"ERROR: Adapter {adapter.name} failed: {e}")
                # Create error result for all tasks
                for task_def in suite.get("tasks", []):
                    task_result = TaskResult(
                        task_id=task_def["id"],
                        adapter_name=adapter.name,
                        errors=[str(e)],
                    )
                    results.task_results.append(task_result)
        
        results.finished_at = datetime.now().isoformat()
        return results
    
    def _run_adapter(
        self,
        adapter: Adapter,
        suite: dict[str, Any],
        manifest: dict[str, Any],
        csv_paths: list[Path],
        results: BenchmarkResults,
    ) -> None:
        """Run all tasks for a single adapter."""
        table_name = manifest.get("table_name", "benchmark")
        
        # Setup adapter
        self._log(f"  Setting up {adapter.name}...")
        schema = {
            "columns": manifest.get("columns", manifest.get("schema", [])),
            "table_name": table_name,
        }
        adapter.setup(schema)
        
        # Check if manifest defines multiple tables
        tables_map = manifest.get("tables")
        dataset_dir = csv_paths[0].parent if csv_paths else None
        
        if tables_map and dataset_dir:
            # Load each table separately
            self._log(f"  Loading {len(tables_map)} table(s)...")
            for tbl_name, csv_file in tables_map.items():
                csv_path = dataset_dir / csv_file
                adapter.load_csv([csv_path], tbl_name)
        else:
            # Single table mode
            self._log(f"  Loading {len(csv_paths)} CSV file(s)...")
            adapter.load_csv(csv_paths, table_name)
        
        # Run each task
        for task_def in suite.get("tasks", []):
            task_result = self._run_task(adapter, suite, task_def)
            results.task_results.append(task_result)
        
        # Cleanup
        adapter.close()
    
    def _run_task(
        self,
        adapter: Adapter,
        suite: dict[str, Any],
        task_def: dict[str, Any],
    ) -> TaskResult:
        """Run a single task with warmup and measured iterations."""
        task_id = task_def["id"]
        params = task_def.get("params", {})
        warmup = task_def.get("warmup", suite.get("warmup", 3))
        iterations = task_def.get("iterations", suite.get("iterations", 10))
        cache_mode = task_def.get("cache_mode", suite.get("cache_mode", "warm"))
        expected_rows = task_def.get("expected_rows")
        expected_checksum = task_def.get("expected_checksum")
        
        description = task_def.get("description", "")
        if description:
            self._log(f"\n  Task: {task_id} - {description}")
        else:
            self._log(f"\n  Task: {task_id}")
        self._log(f"    Cache mode: {cache_mode}, Warmup: {warmup}, Iterations: {iterations}")
        
        task_result = TaskResult(
            task_id=task_id,
            adapter_name=adapter.name,
            cache_mode=cache_mode,
            warmup_iterations=warmup,
            measured_iterations=iterations,
        )
        
        # Warmup iterations (not measured)
        for i in range(warmup):
            if cache_mode == "cold":
                adapter.clear_cache()
            try:
                result = adapter.run(task_id, params)
                if not result.success:
                    self._handle_error(
                        adapter.name, task_id, 
                        result.query, result.error_message,
                        is_warmup=True
                    )
            except Exception as e:
                self._handle_error(adapter.name, task_id, None, str(e), is_warmup=True)
        
        # Measured iterations
        for i in range(iterations):
            if cache_mode == "cold":
                adapter.clear_cache()
            
            try:
                result = adapter.run(task_id, params)
                
                # Store query from first result (even if failed)
                if task_result.query is None and result.query:
                    task_result.query = result.query
                
                if not result.success:
                    task_result.errors.append(result.error_message or "Unknown error")
                    self._handle_error(
                        adapter.name, task_id,
                        result.query, result.error_message
                    )
                    continue
                
                task_result.timings_ns.append(result.execution_time_ns)
                task_result.row_counts.append(result.row_count)
                task_result.checksums.append(result.checksum)
                
            except Exception as e:
                task_result.errors.append(str(e))
                self._handle_error(adapter.name, task_id, task_result.query, str(e))
        
        # Validate results
        if expected_rows is not None and task_result.row_counts:
            # All iterations should return the same row count
            unique_counts = set(task_result.row_counts)
            if len(unique_counts) > 1:
                task_result.validation_passed = False
                task_result.validation_error = f"Inconsistent row counts: {unique_counts}"
            elif expected_rows not in unique_counts:
                task_result.validation_passed = False
                task_result.validation_error = (
                    f"Expected {expected_rows} rows, got {task_result.row_counts[0]}"
                )
        
        if expected_checksum is not None and task_result.checksums:
            valid_checksums = [c for c in task_result.checksums if c is not None]
            if valid_checksums and expected_checksum not in valid_checksums:
                task_result.validation_passed = False
                task_result.validation_error = (
                    f"Expected checksum {expected_checksum}, got {valid_checksums[0]}"
                )
        
        # Log summary
        if task_result.timings_ns:
            times_ms = [t / 1_000_000 for t in task_result.timings_ns]
            median_ms = sorted(times_ms)[len(times_ms) // 2]
            self._log(f"    Median: {median_ms:.2f} ms ({len(task_result.timings_ns)} runs)")
        if task_result.query:
            self._log(f"    Query: {task_result.query}")
        if task_result.errors:
            self._log(f"    Errors: {len(task_result.errors)}")
        if not task_result.validation_passed:
            self._log(f"    VALIDATION FAILED: {task_result.validation_error}")
        
        return task_result
    
    def _get_system_info(self) -> dict[str, Any]:
        """Collect system information for reproducibility."""
        import platform
        
        info = {
            "platform": platform.platform(),
            "python_version": platform.python_version(),
            "processor": platform.processor(),
        }
        
        # Try to get more detailed info with psutil
        try:
            import psutil
            info["cpu_count"] = psutil.cpu_count()
            info["cpu_count_logical"] = psutil.cpu_count(logical=True)
            mem = psutil.virtual_memory()
            info["memory_total_gb"] = round(mem.total / (1024**3), 2)
        except ImportError:
            pass
        
        return info
    
    def _log(self, msg: str) -> None:
        """Print message if verbose mode is enabled."""
        if self.verbose:
            print(msg)
    
    def _handle_error(
        self,
        adapter_name: str,
        task_id: str,
        query: str | None,
        error_message: str | None,
        is_warmup: bool = False,
    ) -> None:
        """Handle task execution error - log and optionally raise."""
        phase = "WARMUP" if is_warmup else "EXECUTION"
        
        print(f"\n{'='*60}")
        print(f"ERROR [{phase}]: {adapter_name} / {task_id}")
        print(f"{'='*60}")
        if query:
            print(f"Query:\n  {query}")
        print(f"Error:\n  {error_message or 'Unknown error'}")
        print(f"{'='*60}\n")
        
        if self.stop_on_error and not is_warmup:
            raise TaskError(f"{adapter_name}/{task_id}: {error_message}")