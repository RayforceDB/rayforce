"""
Polars adapter for benchmarking.

Uses Polars DataFrame library for in-process execution.
Uses lazy evaluation with collect() for optimal parallel execution.
Measures only native query execution time using perf_counter_ns.

FAIRNESS:
- Uses LazyFrame for query optimization (predicate pushdown, projection)
- Times only the collect() call which triggers native Rust execution
- Polars uses all available cores by default
- No Python overhead in the timed section - just native DataFrame ops
"""

import hashlib
import time
from pathlib import Path
from typing import Any

import polars as pl

from benchmarks.adapter import Adapter, AdapterResult, SetupError, TaskError


# Type mapping from manifest types to Polars types
TYPE_MAP = {
    "I64": pl.Int64,
    "I32": pl.Int32,
    "I16": pl.Int16,
    "F64": pl.Float64,
    "F32": pl.Float32,
    "SYMBOL": pl.Utf8,
    "STRING": pl.Utf8,
    "DATE": pl.Date,
    "TIME": pl.Time,
    "TIMESTAMP": pl.Datetime,
    "BOOL": pl.Boolean,
    "B8": pl.Boolean,
}


class PolarsAdapter(Adapter):
    """Polars in-process adapter.

    Uses Polars DataFrame library for high-performance operations.
    All operations run in-process without IPC or network.

    PARALLELISM:
    - Polars uses Rayon for parallel execution (Rust thread pool)
    - By default uses all available CPU cores
    """

    name = "polars"
    version = pl.__version__
    embedded = True

    def __init__(self):
        """Initialize Polars adapter."""
        self._tables: dict[str, pl.DataFrame] = {}
        self._lazy_tables: dict[str, pl.LazyFrame] = {}  # Lazy versions for optimized queries
        self._table_name: str = ""
        self._schema: dict[str, Any] = {}
        self._tasks = self._build_task_registry()
    
    def _build_task_registry(self) -> dict[str, callable]:
        """Build registry of task handlers."""
        return {
            # H2OAI Group By queries
            "groupby_q1": self._task_groupby_q1,
            "groupby_q2": self._task_groupby_q2,
            "groupby_q3": self._task_groupby_q3,
            "groupby_q4": self._task_groupby_q4,
            "groupby_q5": self._task_groupby_q5,
            "groupby_q6": self._task_groupby_q6,
            "groupby_q7": self._task_groupby_q7,
            "groupby_q8": self._task_groupby_q8,
            "groupby_q9": self._task_groupby_q9,
            "groupby_q10": self._task_groupby_q10,
            # Join queries
            "inner_join": self._task_inner_join,
            "left_join": self._task_left_join,
            # Sort queries
            "sort_single": self._task_sort_single,
            "sort_multi": self._task_sort_multi,
            # Window join queries
            "window_join": self._task_window_join,
            # Generic execution
            "sql": self._task_sql,
        }
    
    def setup(self, schema: dict[str, Any]) -> None:
        """Initialize Polars environment."""
        self._schema = schema
        self._table_name = schema.get("table_name", "t")
    
    def load_csv(self, csv_paths: list[Path], table_name: str) -> None:
        """Load CSV files into Polars DataFrame.
        
        Loads data eagerly for consistent memory layout, then creates
        lazy views for query optimization.
        """
        if not csv_paths:
            raise SetupError("No CSV files provided")
        
        self._table_name = table_name
        
        # Skip if already loaded
        if table_name in self._tables:
            return
        
        # Build schema from manifest if available
        schema = None
        if self._schema.get("columns"):
            schema = {}
            for col in self._schema["columns"]:
                col_name = col.get("name")
                col_type = col.get("type", "STRING")
                if col_name:
                    schema[col_name] = TYPE_MAP.get(col_type, pl.Utf8)
        
        # Load CSV(s) - read_csv is already parallelized in Polars
        if len(csv_paths) == 1:
            df = pl.read_csv(csv_paths[0], schema=schema)
        else:
            # Multiple CSV files - parallel load and concat
            dfs = [pl.read_csv(p, schema=schema) for p in csv_paths]
            df = pl.concat(dfs)
        
        # Store both eager and lazy versions
        self._tables[table_name] = df
        self._lazy_tables[table_name] = df.lazy()
    
    def run(self, task: str, params: dict[str, Any]) -> AdapterResult:
        """Execute a benchmark task."""
        handler = self._tasks.get(task)
        if handler is None:
            if "query" in params:
                return self._execute_sql(params["query"])
            raise TaskError(f"Unknown task: {task}")
        
        return handler(params)
    
    def close(self) -> None:
        """Clean up Polars resources."""
        self._tables.clear()
        self._lazy_tables.clear()
    
    def clear_cache(self) -> None:
        """Clear caches for cold-run benchmarks."""
        # Polars doesn't have explicit cache clearing
        pass
    
    def get_info(self) -> dict[str, Any]:
        """Return Polars-specific metadata."""
        info = super().get_info()
        info.update({
            "polars_version": pl.__version__,
        })
        return info
    
    def _get_table(self, name: str) -> pl.DataFrame:
        """Get a loaded eager DataFrame by name."""
        if name not in self._tables:
            raise TaskError(f"Table not loaded: {name}")
        return self._tables[name]
    
    def _get_lazy_table(self, name: str) -> pl.LazyFrame:
        """Get a loaded LazyFrame by name for optimized queries."""
        if name not in self._lazy_tables:
            raise TaskError(f"Table not loaded: {name}")
        return self._lazy_tables[name]
    
    def _execute_lazy(self, query_fn, query_desc: str = "") -> AdapterResult:
        """Execute a lazy query and return result metadata.
        
        FAIRNESS: We time query plan construction + collect() to match other
        databases that include parsing/planning in their timing (DuckDB execute,
        Rayforce timeit, KDB+ \t).
        
        Args:
            query_fn: A callable that builds and returns a LazyFrame query
            query_desc: Description of the query for logging
        """
        try:
            # Time both query plan construction AND execution
            # This matches DuckDB/Rayforce/KDB+ which include parsing+planning
            start_ns = time.perf_counter_ns()
            lazy_frame = query_fn()
            result = lazy_frame.collect()
            end_ns = time.perf_counter_ns()
            
            row_count = len(result)
            
            # Checksum from sample (AFTER timing)
            checksum = None
            if row_count > 0 and len(result.columns) > 0:
                first_col = result.columns[0]
                sample = result.head(100).select(first_col).to_series().to_list()
                checksum = int(hashlib.md5(str(sample).encode()).hexdigest()[:8], 16)
            
            return AdapterResult(
                execution_time_ns=end_ns - start_ns,
                row_count=row_count,
                checksum=checksum,
                query=query_desc,
            )
        except Exception as e:
            return AdapterResult(
                execution_time_ns=0,
                row_count=0,
                success=False,
                error_message=str(e),
                query=query_desc,
            )
    
    def _execute_expr(self, expr_fn, query_desc: str = "") -> AdapterResult:
        """Execute a Polars expression and return result metadata.
        
        DEPRECATED: Use _execute_lazy for new code.
        Kept for backwards compatibility with sql task.
        """
        try:
            start_ns = time.perf_counter_ns()
            result = expr_fn()
            
            # Force evaluation if lazy
            if isinstance(result, pl.LazyFrame):
                result = result.collect()
            
            end_ns = time.perf_counter_ns()
            
            row_count = len(result)
            
            # Checksum from sample
            checksum = None
            if row_count > 0 and len(result.columns) > 0:
                first_col = result.columns[0]
                sample = result.head(100).select(first_col).to_series().to_list()
                checksum = int(hashlib.md5(str(sample).encode()).hexdigest()[:8], 16)
            
            return AdapterResult(
                execution_time_ns=end_ns - start_ns,
                row_count=row_count,
                checksum=checksum,
                query=query_desc,
            )
        except Exception as e:
            return AdapterResult(
                execution_time_ns=0,
                row_count=0,
                success=False,
                error_message=str(e),
                query=query_desc,
            )
    
    def _execute_sql(self, query: str) -> AdapterResult:
        """Execute SQL query using Polars SQL context."""
        try:
            # Register all tables with SQL context
            ctx = pl.SQLContext()
            for name, df in self._tables.items():
                ctx.register(name, df)
            
            start_ns = time.perf_counter_ns()
            result = ctx.execute(query).collect()
            end_ns = time.perf_counter_ns()
            
            row_count = len(result)
            
            # Checksum
            checksum = None
            if row_count > 0 and len(result.columns) > 0:
                first_col = result.columns[0]
                sample = result.head(100).select(first_col).to_series().to_list()
                checksum = int(hashlib.md5(str(sample).encode()).hexdigest()[:8], 16)
            
            return AdapterResult(
                execution_time_ns=end_ns - start_ns,
                row_count=row_count,
                checksum=checksum,
                query=query,
            )
        except Exception as e:
            return AdapterResult(
                execution_time_ns=0,
                row_count=0,
                success=False,
                error_message=str(e),
                query=query,
            )
    
    # =========================================================================
    # H2OAI Group By Queries (using lazy evaluation for optimal parallelism)
    # All queries return grouping columns + aggregated values (matching Rayforce)
    # Timing includes query plan construction + execution (matching other DBs)
    # =========================================================================
    
    def _task_groupby_q1(self, params: dict[str, Any]) -> AdapterResult:
        """Q1: sum(v1) by id1 -> returns [id1, v1]"""
        table_name = params.get("table", self._table_name)
        lf = self._get_lazy_table(table_name)
        
        def query():
            return lf.group_by("id1").agg(pl.sum("v1").alias("v1"))
        
        return self._execute_lazy(query, 'lf.group_by("id1").agg(sum(v1))')
    
    def _task_groupby_q2(self, params: dict[str, Any]) -> AdapterResult:
        """Q2: sum(v1) by id1, id2 -> returns [id1, id2, v1]"""
        table_name = params.get("table", self._table_name)
        lf = self._get_lazy_table(table_name)
        
        def query():
            return lf.group_by(["id1", "id2"]).agg(pl.sum("v1").alias("v1"))
        
        return self._execute_lazy(query, 'lf.group_by(["id1","id2"]).agg(sum(v1))')
    
    def _task_groupby_q3(self, params: dict[str, Any]) -> AdapterResult:
        """Q3: sum(v1), avg(v3) by id3 -> returns [id3, v1, v3]"""
        table_name = params.get("table", self._table_name)
        lf = self._get_lazy_table(table_name)
        
        def query():
            return lf.group_by("id3").agg([
                pl.sum("v1").alias("v1"),
                pl.mean("v3").alias("v3"),
            ])
        
        return self._execute_lazy(query, 'lf.group_by("id3").agg([sum(v1), mean(v3)])')
    
    def _task_groupby_q4(self, params: dict[str, Any]) -> AdapterResult:
        """Q4: avg(v1), avg(v2), avg(v3) by id4 -> returns [id4, v1, v2, v3]"""
        table_name = params.get("table", self._table_name)
        lf = self._get_lazy_table(table_name)
        
        def query():
            return lf.group_by("id4").agg([
                pl.mean("v1").alias("v1"),
                pl.mean("v2").alias("v2"),
                pl.mean("v3").alias("v3"),
            ])
        
        return self._execute_lazy(query, 'lf.group_by("id4").agg([mean(v1), mean(v2), mean(v3)])')
    
    def _task_groupby_q5(self, params: dict[str, Any]) -> AdapterResult:
        """Q5: sum(v1), sum(v2), sum(v3) by id6 -> returns [id6, v1, v2, v3]"""
        table_name = params.get("table", self._table_name)
        lf = self._get_lazy_table(table_name)
        
        def query():
            return lf.group_by("id6").agg([
                pl.sum("v1").alias("v1"),
                pl.sum("v2").alias("v2"),
                pl.sum("v3").alias("v3"),
            ])
        
        return self._execute_lazy(query, 'lf.group_by("id6").agg([sum(v1), sum(v2), sum(v3)])')
    
    def _task_groupby_q6(self, params: dict[str, Any]) -> AdapterResult:
        """Q6: max(v1) - min(v2) by id3 -> returns [id3, range_v1_v2]"""
        table_name = params.get("table", self._table_name)
        lf = self._get_lazy_table(table_name)
        
        def query():
            return lf.group_by("id3").agg(
                (pl.max("v1") - pl.min("v2")).alias("range_v1_v2")
            )
        
        return self._execute_lazy(query, 'lf.group_by("id3").agg(max(v1) - min(v2))')
    
    def _task_groupby_q7(self, params: dict[str, Any]) -> AdapterResult:
        """Q7: sum(v3), count by id1-id6 -> returns [id1, id2, id3, id4, id5, id6, v3, count]"""
        table_name = params.get("table", self._table_name)
        lf = self._get_lazy_table(table_name)
        
        def query():
            return lf.group_by(["id1", "id2", "id3", "id4", "id5", "id6"]).agg([
                pl.sum("v3").alias("v3"),
                pl.len().alias("count"),
            ])
        
        return self._execute_lazy(query, 'lf.group_by([id1-id6]).agg([sum(v3), len()])')
    
    def _task_groupby_q8(self, params: dict[str, Any]) -> AdapterResult:
        """Q8: Range filter + aggregation: sum(v3) by id2 where v1 >= 3"""
        table_name = params.get("table", self._table_name)
        lf = self._get_lazy_table(table_name)
        
        def query():
            return (
                lf.filter(pl.col("v1") >= 3)
                .group_by("id2")
                .agg(pl.sum("v3").alias("v3"))
            )
        
        return self._execute_lazy(query, 'lf.filter(v1>=3).group_by("id2").agg(sum(v3))')
    
    def _task_groupby_q9(self, params: dict[str, Any]) -> AdapterResult:
        """Q9: Compound filter + multi-agg: sum(v1,v2,v3) by id3 where v1>=2 AND v2<=8"""
        table_name = params.get("table", self._table_name)
        lf = self._get_lazy_table(table_name)
        
        def query():
            return (
                lf.filter((pl.col("v1") >= 2) & (pl.col("v2") <= 8))
                .group_by("id3")
                .agg([
                    pl.sum("v1").alias("v1"),
                    pl.sum("v2").alias("v2"),
                    pl.sum("v3").alias("v3"),
                ])
            )
        
        return self._execute_lazy(query, 'lf.filter(v1>=2 & v2<=8).group_by("id3").agg([sum(v1,v2,v3)])')
    
    def _task_groupby_q10(self, params: dict[str, Any]) -> AdapterResult:
        """Q10: Filter + group: sum(v1), sum(v2) by id1-id4 where v3>0"""
        table_name = params.get("table", self._table_name)
        lf = self._get_lazy_table(table_name)
        
        def query():
            return (
                lf.filter(pl.col("v3") > 0)
                .group_by(["id1", "id2", "id3", "id4"])
                .agg([
                    pl.sum("v1").alias("v1"),
                    pl.sum("v2").alias("v2"),
                ])
            )
        
        return self._execute_lazy(query, 'lf.filter(v3>0).group_by([id1-id4]).agg([sum(v1), sum(v2)])')
    
    # =========================================================================
    # Join Queries (using lazy evaluation for optimal parallelism)
    # Timing includes query plan construction + execution (matching other DBs)
    # =========================================================================
    
    def _task_inner_join(self, params: dict[str, Any]) -> AdapterResult:
        """Inner join on id1, id2"""
        left_table = params.get("left_table", "x")
        right_table = params.get("right_table", "y")
        
        left_lf = self._get_lazy_table(left_table)
        right_lf = self._get_lazy_table(right_table)
        
        def query():
            return left_lf.join(right_lf, on=["id1", "id2"], how="inner")
        
        return self._execute_lazy(query, f'{left_table}.join({right_table}, on=["id1","id2"], how="inner")')
    
    def _task_left_join(self, params: dict[str, Any]) -> AdapterResult:
        """Left join on id1, id2"""
        left_table = params.get("left_table", "x")
        right_table = params.get("right_table", "y")

        left_lf = self._get_lazy_table(left_table)
        right_lf = self._get_lazy_table(right_table)

        def query():
            return left_lf.join(right_lf, on=["id1", "id2"], how="left")

        return self._execute_lazy(query, f'{left_table}.join({right_table}, on=["id1","id2"], how="left")')

    # =========================================================================
    # Sort Queries (using lazy evaluation for optimal parallelism)
    # =========================================================================

    def _task_sort_single(self, params: dict[str, Any]) -> AdapterResult:
        """Sort by single column"""
        table_name = params.get("table", self._table_name)
        column = params.get("column", "id1")
        descending = params.get("descending", False)
        lf = self._get_lazy_table(table_name)

        def query():
            return lf.sort(column, descending=descending)

        return self._execute_lazy(query, f'lf.sort("{column}", descending={descending})')

    def _task_sort_multi(self, params: dict[str, Any]) -> AdapterResult:
        """Sort by multiple columns"""
        table_name = params.get("table", self._table_name)
        columns = params.get("columns", ["id1", "id2"])
        lf = self._get_lazy_table(table_name)

        def query():
            return lf.sort(columns)

        return self._execute_lazy(query, f'lf.sort({columns})')

    # =========================================================================
    # Window Join Queries (using join_asof for time-series joins)
    # =========================================================================

    def _task_window_join(self, params: dict[str, Any]) -> AdapterResult:
        """Window join - join within time window with aggregations

        Polars doesn't have a direct wj1 equivalent, so we use a range join
        approach with group_by for aggregation.
        """
        import datetime

        trades_table = params.get("trades_table", "trades")
        quotes_table = params.get("quotes_table", "quotes")
        window_ms = params.get("window_ms", 10000)  # +/- 10 seconds default

        trades_lf = self._get_lazy_table(trades_table)
        quotes_lf = self._get_lazy_table(quotes_table)

        def query():
            # Add window boundaries to trades
            trades_with_window = trades_lf.with_columns([
                (pl.col("Ts") - datetime.timedelta(milliseconds=window_ms)).alias("_window_start"),
                (pl.col("Ts") + datetime.timedelta(milliseconds=window_ms)).alias("_window_end"),
            ])

            # Cross join on Sym, filter by time window, then aggregate
            # This is expensive but semantically correct
            result = (
                trades_with_window
                .join(quotes_lf, on="Sym", how="left")
                .filter(
                    (pl.col("Ts_right") >= pl.col("_window_start")) &
                    (pl.col("Ts_right") <= pl.col("_window_end"))
                )
                .group_by(["Sym", "Ts", "Price"])
                .agg([
                    pl.min("Bid").alias("Bid"),
                    pl.max("Ask").alias("Ask"),
                ])
            )
            return result

        return self._execute_lazy(query, 'window_join(trades, quotes)')

    def _task_sql(self, params: dict[str, Any]) -> AdapterResult:
        """Execute arbitrary SQL query."""
        query = params.get("query")
        if not query:
            return AdapterResult(
                execution_time_ns=0,
                row_count=0,
                success=False,
                error_message="No query provided",
            )
        return self._execute_sql(query)
