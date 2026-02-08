"""
QuestDB adapter for benchmarking.

Uses QuestDB's PostgreSQL wire protocol for SQL execution.
Measures query execution time using perf_counter_ns.

FAIRNESS:
- Uses standard SQL execution via psycopg2
- Times only the query execution, not connection setup
- Data is loaded into QuestDB before benchmarking begins

Note: Requires a running QuestDB instance and psycopg2 library.
"""

import hashlib
import time
from pathlib import Path
from typing import Any

try:
    import psycopg2
    PSYCOPG2_AVAILABLE = True
except ImportError:
    PSYCOPG2_AVAILABLE = False

from benchmarks.adapter import Adapter, AdapterResult, SetupError, TaskError
from benchmarks.config import get_config


# Type mapping from manifest types to QuestDB types
TYPE_MAP = {
    "I64": "LONG",
    "I32": "INT",
    "I16": "SHORT",
    "F64": "DOUBLE",
    "F32": "FLOAT",
    "SYMBOL": "SYMBOL",
    "STRING": "STRING",
    "DATE": "DATE",
    "TIME": "TIMESTAMP",
    "TIMESTAMP": "TIMESTAMP",
    "BOOL": "BOOLEAN",
    "B8": "BOOLEAN",
}


class QuestDBAdapter(Adapter):
    """QuestDB adapter using PostgreSQL wire protocol.

    Connects to a running QuestDB instance via its PostgreSQL interface.
    All operations use SQL queries over the wire.
    """

    name = "questdb"
    version = "7.0"  # QuestDB version
    embedded = False

    def __init__(
        self,
        host: str | None = None,
        port: int | None = None,
        database: str | None = None,
        user: str | None = None,
        password: str | None = None,
    ):
        """Initialize QuestDB adapter.

        Args:
            host: QuestDB host (default: localhost).
            port: PostgreSQL wire port (default: 8812).
            database: Database name (default: qdb).
            user: Username (default: admin).
            password: Password (default: quest).
        """
        if not PSYCOPG2_AVAILABLE:
            raise SetupError("psycopg2 not installed. Run: pip install psycopg2-binary")

        config = get_config()
        qdb_config = config.get_section("questdb")

        self.host = host or qdb_config.get("host", "localhost")
        self.port = port or qdb_config.get("port", 8812)
        self.database = database or qdb_config.get("database", "qdb")
        self.user = user or qdb_config.get("user", "admin")
        self.password = password or qdb_config.get("password", "quest")

        self._conn = None
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
            # Generic SQL
            "sql": self._task_sql,
        }

    def setup(self, schema: dict[str, Any]) -> None:
        """Initialize QuestDB connection."""
        self._schema = schema
        self._table_name = schema.get("table_name", "benchmark")

        try:
            self._conn = psycopg2.connect(
                host=self.host,
                port=self.port,
                database=self.database,
                user=self.user,
                password=self.password,
            )
            # QuestDB requires autocommit mode
            self._conn.autocommit = True
        except Exception as e:
            raise SetupError(f"Failed to connect to QuestDB at {self.host}:{self.port}: {e}")

    def load_csv(self, csv_paths: list[Path], table_name: str) -> None:
        """Load CSV files into QuestDB table via REST API.

        QuestDB's /imp endpoint requires multipart/form-data.
        """
        import http.client
        import mimetypes
        import uuid

        if not csv_paths:
            raise SetupError("No CSV files provided")

        self._table_name = table_name

        # Drop table if exists (via SQL)
        if self._conn:
            with self._conn.cursor() as cur:
                try:
                    cur.execute(f"DROP TABLE IF EXISTS {table_name}")
                except Exception:
                    pass

        # Use REST API to import CSV with multipart/form-data
        rest_port = 9000  # QuestDB REST API default port

        for csv_path in csv_paths:
            try:
                boundary = str(uuid.uuid4())

                with open(csv_path, 'rb') as f:
                    csv_data = f.read()

                # Build multipart form data
                body = (
                    f'--{boundary}\r\n'
                    f'Content-Disposition: form-data; name="data"; filename="{table_name}.csv"\r\n'
                    f'Content-Type: text/csv\r\n\r\n'
                ).encode('utf-8') + csv_data + f'\r\n--{boundary}--\r\n'.encode('utf-8')

                conn = http.client.HTTPConnection(self.host, rest_port, timeout=300)
                conn.request(
                    'POST',
                    f'/imp?name={table_name}&overwrite=true&header=true',
                    body=body,
                    headers={
                        'Content-Type': f'multipart/form-data; boundary={boundary}',
                        'Content-Length': str(len(body)),
                    }
                )

                response = conn.getresponse()
                response_body = response.read().decode('utf-8')
                conn.close()

                if response.status != 200:
                    raise SetupError(f"Failed to import CSV (HTTP {response.status}): {response_body}")

            except SetupError:
                raise
            except Exception as e:
                raise SetupError(f"Failed to load CSV via REST API: {e}")

    def run(self, task: str, params: dict[str, Any]) -> AdapterResult:
        """Execute a benchmark task."""
        if not self._conn:
            raise TaskError("Adapter not initialized")

        handler = self._tasks.get(task)
        if handler is None:
            if "query" in params:
                return self._execute_query(params["query"])
            raise TaskError(f"Unknown task: {task}")

        return handler(params)

    def close(self) -> None:
        """Close QuestDB connection."""
        if self._conn:
            self._conn.close()
            self._conn = None

    def clear_cache(self) -> None:
        """QuestDB manages caching internally."""
        pass

    def get_info(self) -> dict[str, Any]:
        """Return QuestDB-specific metadata."""
        info = super().get_info()
        info.update({
            "questdb_version": self.version,
            "host": self.host,
            "port": self.port,
        })
        return info

    def _execute_query(self, query: str) -> AdapterResult:
        """Execute a SQL query and return result metadata."""
        try:
            with self._conn.cursor() as cur:
                start_ns = time.perf_counter_ns()
                cur.execute(query)
                rows = cur.fetchall()
                end_ns = time.perf_counter_ns()

                row_count = len(rows)

                # Checksum from sample
                checksum = None
                if rows and len(rows[0]) > 0:
                    sample = rows[:100]
                    first_col_str = str([row[0] for row in sample])
                    checksum = int(hashlib.md5(first_col_str.encode()).hexdigest()[:8], 16)

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
    # H2OAI Group By Queries (SQL syntax)
    # =========================================================================

    def _task_groupby_q1(self, params: dict[str, Any]) -> AdapterResult:
        """Q1: sum(v1) by id1"""
        table = params.get("table", self._table_name)
        query = f"SELECT id1, SUM(v1) AS v1 FROM {table} GROUP BY id1"
        return self._execute_query(query)

    def _task_groupby_q2(self, params: dict[str, Any]) -> AdapterResult:
        """Q2: sum(v1) by id1, id2"""
        table = params.get("table", self._table_name)
        query = f"SELECT id1, id2, SUM(v1) AS v1 FROM {table} GROUP BY id1, id2"
        return self._execute_query(query)

    def _task_groupby_q3(self, params: dict[str, Any]) -> AdapterResult:
        """Q3: sum(v1), avg(v3) by id3"""
        table = params.get("table", self._table_name)
        query = f"SELECT id3, SUM(v1) AS v1, AVG(v3) AS v3 FROM {table} GROUP BY id3"
        return self._execute_query(query)

    def _task_groupby_q4(self, params: dict[str, Any]) -> AdapterResult:
        """Q4: avg(v1), avg(v2), avg(v3) by id4"""
        table = params.get("table", self._table_name)
        query = f"SELECT id4, AVG(v1) AS v1, AVG(v2) AS v2, AVG(v3) AS v3 FROM {table} GROUP BY id4"
        return self._execute_query(query)

    def _task_groupby_q5(self, params: dict[str, Any]) -> AdapterResult:
        """Q5: sum(v1), sum(v2), sum(v3) by id6"""
        table = params.get("table", self._table_name)
        query = f"SELECT id6, SUM(v1) AS v1, SUM(v2) AS v2, SUM(v3) AS v3 FROM {table} GROUP BY id6"
        return self._execute_query(query)

    def _task_groupby_q6(self, params: dict[str, Any]) -> AdapterResult:
        """Q6: max(v1) - min(v2) by id3"""
        table = params.get("table", self._table_name)
        query = f"SELECT id3, MAX(v1) - MIN(v2) AS range_v1_v2 FROM {table} GROUP BY id3"
        return self._execute_query(query)

    def _task_groupby_q7(self, params: dict[str, Any]) -> AdapterResult:
        """Q7: sum(v3), count by id1-id6"""
        table = params.get("table", self._table_name)
        query = f"""
            SELECT id1, id2, id3, id4, id5, id6,
                   SUM(v3) AS v3, COUNT(*) AS count
            FROM {table}
            GROUP BY id1, id2, id3, id4, id5, id6
        """
        return self._execute_query(query)

    def _task_groupby_q8(self, params: dict[str, Any]) -> AdapterResult:
        """Q8: Range filter + aggregation: sum(v3) by id2 where v1 >= 3"""
        table = params.get("table", self._table_name)
        query = f"""
            SELECT id2, SUM(v3) AS v3
            FROM {table}
            WHERE v1 >= 3
            GROUP BY id2
        """
        return self._execute_query(query)

    def _task_groupby_q9(self, params: dict[str, Any]) -> AdapterResult:
        """Q9: Compound filter + multi-agg: sum(v1,v2,v3) by id3 where v1>=2 AND v2<=8"""
        table = params.get("table", self._table_name)
        query = f"""
            SELECT id3, SUM(v1) AS v1, SUM(v2) AS v2, SUM(v3) AS v3
            FROM {table}
            WHERE v1 >= 2 AND v2 <= 8
            GROUP BY id3
        """
        return self._execute_query(query)

    def _task_groupby_q10(self, params: dict[str, Any]) -> AdapterResult:
        """Q10: Filter + group: sum(v1), sum(v2) by id1-id4 where v3>0"""
        table = params.get("table", self._table_name)
        query = f"""
            SELECT id1, id2, id3, id4, SUM(v1) AS v1, SUM(v2) AS v2
            FROM {table}
            WHERE v3 > 0
            GROUP BY id1, id2, id3, id4
        """
        return self._execute_query(query)

    # =========================================================================
    # Join Queries (SQL syntax)
    # =========================================================================

    def _task_inner_join(self, params: dict[str, Any]) -> AdapterResult:
        """Inner join on id1, id2"""
        left_table = params.get("left_table", "x")
        right_table = params.get("right_table", "y")
        query = f"""
            SELECT * FROM {left_table}
            INNER JOIN {right_table}
            ON {left_table}.id1 = {right_table}.id1
            AND {left_table}.id2 = {right_table}.id2
        """
        return self._execute_query(query)

    def _task_left_join(self, params: dict[str, Any]) -> AdapterResult:
        """Left join on id1, id2"""
        left_table = params.get("left_table", "x")
        right_table = params.get("right_table", "y")
        query = f"""
            SELECT * FROM {left_table}
            LEFT JOIN {right_table}
            ON {left_table}.id1 = {right_table}.id1
            AND {left_table}.id2 = {right_table}.id2
        """
        return self._execute_query(query)

    # =========================================================================
    # Sort Queries (SQL syntax)
    # =========================================================================

    def _task_sort_single(self, params: dict[str, Any]) -> AdapterResult:
        """Sort by single column"""
        table = params.get("table", self._table_name)
        column = params.get("column", "id1")
        descending = params.get("descending", False)
        order = "DESC" if descending else "ASC"
        query = f"SELECT * FROM {table} ORDER BY {column} {order}"
        return self._execute_query(query)

    def _task_sort_multi(self, params: dict[str, Any]) -> AdapterResult:
        """Sort by multiple columns"""
        table = params.get("table", self._table_name)
        columns = params.get("columns", ["id1", "id2"])
        order_by = ", ".join(columns)
        query = f"SELECT * FROM {table} ORDER BY {order_by}"
        return self._execute_query(query)

    # =========================================================================
    # Window Join Queries
    # QuestDB has native ASOF JOIN support for time-series data
    # =========================================================================

    def _task_window_join(self, params: dict[str, Any]) -> AdapterResult:
        """Window join using QuestDB's ASOF JOIN or range join

        QuestDB has excellent support for time-series joins via ASOF JOIN
        and SPLICE JOIN for temporal data.
        """
        trades_table = params.get("trades_table", "trades")
        quotes_table = params.get("quotes_table", "quotes")
        window_ms = params.get("window_ms", 10000)  # +/- 10 seconds default

        # QuestDB range join with aggregation
        query = f"""
            SELECT
                t.Sym,
                t.Ts,
                t.Price,
                MIN(q.Bid) as Bid,
                MAX(q.Ask) as Ask
            FROM {trades_table} t
            LEFT JOIN {quotes_table} q
                ON t.Sym = q.Sym
                AND q.Ts BETWEEN dateadd('ms', -{window_ms}, t.Ts)
                            AND dateadd('ms', {window_ms}, t.Ts)
            GROUP BY t.Sym, t.Ts, t.Price
        """
        return self._execute_query(query)

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
        return self._execute_query(query)
