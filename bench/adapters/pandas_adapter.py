"""
Pandas adapter for benchmarking.

Uses Pandas DataFrame library for in-process execution.
Measures query execution time using perf_counter_ns.

FAIRNESS:
- Uses standard Pandas operations
- Times only the query execution, not data loading
- No external overhead in the timed section
"""

import hashlib
import time
from pathlib import Path
from typing import Any

import pandas as pd

from benchmarks.adapter import Adapter, AdapterResult, SetupError, TaskError


# Type mapping from manifest types to Pandas/numpy types
TYPE_MAP = {
    "I64": "int64",
    "I32": "int32",
    "I16": "int16",
    "F64": "float64",
    "F32": "float32",
    "SYMBOL": "category",
    "STRING": "object",
    "DATE": "object",  # Will parse as datetime
    "TIME": "object",
    "TIMESTAMP": "datetime64[ns]",
    "BOOL": "bool",
    "B8": "bool",
}


class PandasAdapter(Adapter):
    """Pandas in-process adapter.

    Uses Pandas DataFrame library for data operations.
    All operations run in-process without IPC or network.
    """

    name = "pandas"
    version = pd.__version__
    embedded = True

    def __init__(self):
        """Initialize Pandas adapter."""
        self._tables: dict[str, pd.DataFrame] = {}
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
        }

    def setup(self, schema: dict[str, Any]) -> None:
        """Initialize Pandas environment."""
        self._schema = schema
        self._table_name = schema.get("table_name", "t")

    def load_csv(self, csv_paths: list[Path], table_name: str) -> None:
        """Load CSV files into Pandas DataFrame."""
        if not csv_paths:
            raise SetupError("No CSV files provided")

        self._table_name = table_name

        # Skip if already loaded
        if table_name in self._tables:
            return

        # Build dtype dict from manifest if available
        dtype = None
        if self._schema.get("columns"):
            dtype = {}
            for col in self._schema["columns"]:
                col_name = col.get("name")
                col_type = col.get("type", "STRING")
                if col_name:
                    dtype[col_name] = TYPE_MAP.get(col_type, "object")

        # Load CSV(s)
        if len(csv_paths) == 1:
            df = pd.read_csv(csv_paths[0], dtype=dtype)
        else:
            # Multiple CSV files - load and concat
            dfs = [pd.read_csv(p, dtype=dtype) for p in csv_paths]
            df = pd.concat(dfs, ignore_index=True)

        self._tables[table_name] = df

    def run(self, task: str, params: dict[str, Any]) -> AdapterResult:
        """Execute a benchmark task."""
        handler = self._tasks.get(task)
        if handler is None:
            raise TaskError(f"Unknown task: {task}")

        return handler(params)

    def close(self) -> None:
        """Clean up Pandas resources."""
        self._tables.clear()

    def clear_cache(self) -> None:
        """Clear caches for cold-run benchmarks."""
        pass

    def get_info(self) -> dict[str, Any]:
        """Return Pandas-specific metadata."""
        info = super().get_info()
        info.update({
            "pandas_version": pd.__version__,
        })
        return info

    def _get_table(self, name: str) -> pd.DataFrame:
        """Get a loaded DataFrame by name."""
        if name not in self._tables:
            raise TaskError(f"Table not loaded: {name}")
        return self._tables[name]

    def _execute_op(self, op_fn, query_desc: str = "") -> AdapterResult:
        """Execute an operation and return result metadata."""
        try:
            start_ns = time.perf_counter_ns()
            result = op_fn()
            end_ns = time.perf_counter_ns()

            row_count = len(result)

            # Checksum from sample (AFTER timing)
            checksum = None
            if row_count > 0 and len(result.columns) > 0:
                first_col = result.columns[0]
                sample = result.head(100)[first_col].tolist()
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

    # =========================================================================
    # H2OAI Group By Queries
    # =========================================================================

    def _task_groupby_q1(self, params: dict[str, Any]) -> AdapterResult:
        """Q1: sum(v1) by id1"""
        table_name = params.get("table", self._table_name)
        df = self._get_table(table_name)

        def query():
            return df.groupby("id1", as_index=False).agg(v1=("v1", "sum"))

        return self._execute_op(query, 'df.groupby("id1").agg(v1=("v1", "sum"))')

    def _task_groupby_q2(self, params: dict[str, Any]) -> AdapterResult:
        """Q2: sum(v1) by id1, id2"""
        table_name = params.get("table", self._table_name)
        df = self._get_table(table_name)

        def query():
            return df.groupby(["id1", "id2"], as_index=False).agg(v1=("v1", "sum"))

        return self._execute_op(query, 'df.groupby(["id1","id2"]).agg(v1=("v1", "sum"))')

    def _task_groupby_q3(self, params: dict[str, Any]) -> AdapterResult:
        """Q3: sum(v1), avg(v3) by id3"""
        table_name = params.get("table", self._table_name)
        df = self._get_table(table_name)

        def query():
            return df.groupby("id3", as_index=False).agg(
                v1=("v1", "sum"),
                v3=("v3", "mean"),
            )

        return self._execute_op(query, 'df.groupby("id3").agg([sum(v1), mean(v3)])')

    def _task_groupby_q4(self, params: dict[str, Any]) -> AdapterResult:
        """Q4: avg(v1), avg(v2), avg(v3) by id4"""
        table_name = params.get("table", self._table_name)
        df = self._get_table(table_name)

        def query():
            return df.groupby("id4", as_index=False).agg(
                v1=("v1", "mean"),
                v2=("v2", "mean"),
                v3=("v3", "mean"),
            )

        return self._execute_op(query, 'df.groupby("id4").agg([mean(v1), mean(v2), mean(v3)])')

    def _task_groupby_q5(self, params: dict[str, Any]) -> AdapterResult:
        """Q5: sum(v1), sum(v2), sum(v3) by id6"""
        table_name = params.get("table", self._table_name)
        df = self._get_table(table_name)

        def query():
            return df.groupby("id6", as_index=False).agg(
                v1=("v1", "sum"),
                v2=("v2", "sum"),
                v3=("v3", "sum"),
            )

        return self._execute_op(query, 'df.groupby("id6").agg([sum(v1), sum(v2), sum(v3)])')

    def _task_groupby_q6(self, params: dict[str, Any]) -> AdapterResult:
        """Q6: max(v1) - min(v2) by id3"""
        table_name = params.get("table", self._table_name)
        df = self._get_table(table_name)

        def query():
            result = df.groupby("id3", as_index=False).agg(
                max_v1=("v1", "max"),
                min_v2=("v2", "min"),
            )
            result["range_v1_v2"] = result["max_v1"] - result["min_v2"]
            return result[["id3", "range_v1_v2"]]

        return self._execute_op(query, 'df.groupby("id3").agg(max(v1) - min(v2))')

    def _task_groupby_q7(self, params: dict[str, Any]) -> AdapterResult:
        """Q7: sum(v3), count by id1-id6"""
        table_name = params.get("table", self._table_name)
        df = self._get_table(table_name)

        def query():
            return df.groupby(
                ["id1", "id2", "id3", "id4", "id5", "id6"], as_index=False
            ).agg(
                v3=("v3", "sum"),
                count=("v3", "count"),
            )

        return self._execute_op(query, 'df.groupby([id1-id6]).agg([sum(v3), count()])')

    def _task_groupby_q8(self, params: dict[str, Any]) -> AdapterResult:
        """Q8: Range filter + aggregation: sum(v3) by id2 where v1 >= 3"""
        table_name = params.get("table", self._table_name)
        df = self._get_table(table_name)

        def query():
            return df[df["v1"] >= 3].groupby("id2", as_index=False).agg(v3=("v3", "sum"))

        return self._execute_op(query, 'df[v1>=3].groupby("id2").agg(v3=("v3", "sum"))')

    def _task_groupby_q9(self, params: dict[str, Any]) -> AdapterResult:
        """Q9: Compound filter + multi-agg: sum(v1,v2,v3) by id3 where v1>=2 AND v2<=8"""
        table_name = params.get("table", self._table_name)
        df = self._get_table(table_name)

        def query():
            filtered = df[(df["v1"] >= 2) & (df["v2"] <= 8)]
            return filtered.groupby("id3", as_index=False).agg(
                v1=("v1", "sum"),
                v2=("v2", "sum"),
                v3=("v3", "sum"),
            )

        return self._execute_op(query, 'df[v1>=2 & v2<=8].groupby("id3").agg([sum(v1,v2,v3)])')

    def _task_groupby_q10(self, params: dict[str, Any]) -> AdapterResult:
        """Q10: Filter + group: sum(v1), sum(v2) by id1-id4 where v3>0"""
        table_name = params.get("table", self._table_name)
        df = self._get_table(table_name)

        def query():
            filtered = df[df["v3"] > 0]
            return filtered.groupby(
                ["id1", "id2", "id3", "id4"], as_index=False
            ).agg(
                v1=("v1", "sum"),
                v2=("v2", "sum"),
            )

        return self._execute_op(query, 'df[v3>0].groupby([id1-id4]).agg([sum(v1), sum(v2)])')

    # =========================================================================
    # Join Queries
    # =========================================================================

    def _task_inner_join(self, params: dict[str, Any]) -> AdapterResult:
        """Inner join on id1, id2"""
        left_table = params.get("left_table", "x")
        right_table = params.get("right_table", "y")

        left_df = self._get_table(left_table)
        right_df = self._get_table(right_table)

        def query():
            return pd.merge(left_df, right_df, on=["id1", "id2"], how="inner")

        return self._execute_op(query, f'pd.merge({left_table}, {right_table}, on=["id1","id2"], how="inner")')

    def _task_left_join(self, params: dict[str, Any]) -> AdapterResult:
        """Left join on id1, id2"""
        left_table = params.get("left_table", "x")
        right_table = params.get("right_table", "y")

        left_df = self._get_table(left_table)
        right_df = self._get_table(right_table)

        def query():
            return pd.merge(left_df, right_df, on=["id1", "id2"], how="left")

        return self._execute_op(query, f'pd.merge({left_table}, {right_table}, on=["id1","id2"], how="left")')

    # =========================================================================
    # Sort Queries
    # =========================================================================

    def _task_sort_single(self, params: dict[str, Any]) -> AdapterResult:
        """Sort by single column"""
        table_name = params.get("table", self._table_name)
        column = params.get("column", "id1")
        descending = params.get("descending", False)
        df = self._get_table(table_name)

        def query():
            return df.sort_values(column, ascending=not descending)

        return self._execute_op(query, f'df.sort_values("{column}", ascending={not descending})')

    def _task_sort_multi(self, params: dict[str, Any]) -> AdapterResult:
        """Sort by multiple columns"""
        table_name = params.get("table", self._table_name)
        columns = params.get("columns", ["id1", "id2"])
        df = self._get_table(table_name)

        def query():
            return df.sort_values(columns)

        return self._execute_op(query, f'df.sort_values({columns})')

    # =========================================================================
    # Window Join Queries
    # =========================================================================

    def _task_window_join(self, params: dict[str, Any]) -> AdapterResult:
        """Window join - join within time window with aggregations

        Pandas doesn't have a direct wj1 equivalent, so we use merge_asof
        or a range join approach.
        """
        import datetime

        trades_table = params.get("trades_table", "trades")
        quotes_table = params.get("quotes_table", "quotes")
        window_ms = params.get("window_ms", 10000)  # +/- 10 seconds default

        trades_df = self._get_table(trades_table)
        quotes_df = self._get_table(quotes_table)

        def query():
            # Add window boundaries to trades
            window_delta = pd.Timedelta(milliseconds=window_ms)
            trades_with_window = trades_df.copy()
            trades_with_window["_window_start"] = trades_df["Ts"] - window_delta
            trades_with_window["_window_end"] = trades_df["Ts"] + window_delta

            # Cross join on Sym, filter by time window, then aggregate
            merged = pd.merge(trades_with_window, quotes_df, on="Sym", suffixes=("", "_q"))
            filtered = merged[
                (merged["Ts_q"] >= merged["_window_start"]) &
                (merged["Ts_q"] <= merged["_window_end"])
            ]

            result = filtered.groupby(["Sym", "Ts", "Price"], as_index=False).agg(
                Bid=("Bid", "min"),
                Ask=("Ask", "max"),
            )
            return result

        return self._execute_op(query, 'window_join(trades, quotes)')
