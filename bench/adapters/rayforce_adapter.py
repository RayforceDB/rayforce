"""
RayforceDB adapter for benchmarking.

Supports two modes:
1. IPC mode (preferred): Connect to running rayforce server, data stays loaded
2. Subprocess mode (fallback): Spawn new process per query, reloads CSV each time

IPC mode is fairer for benchmarking as data remains in memory like DuckDB/Polars.
"""

import hashlib
import socket
import struct
import subprocess
import time
from pathlib import Path
from typing import Any

from benchmarks.adapter import Adapter, AdapterResult, SetupError, TaskError
from benchmarks.config import get_config


# IPC Protocol constants
SERDE_PREFIX = 0xcefadefa
MSG_TYPE_ASYNC = 0
MSG_TYPE_SYNC = 1
MSG_TYPE_RESP = 2
HEADER_SIZE = 16  # 4 + 1 + 1 + 1 + 1 + 8 bytes


class RayforceIPCClient:
    """Simple IPC client for Rayforce using string messages."""
    
    def __init__(self, host: str = "127.0.0.1", port: int = 5110, timeout: float = 30.0):
        self.host = host
        self.port = port
        self.timeout = timeout
        self._sock: socket.socket | None = None
        self._version = 0x01  # Protocol version
    
    def connect(self) -> None:
        """Connect to rayforce server and perform handshake."""
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._sock.settimeout(self.timeout)
        self._sock.connect((self.host, self.port))
        
        # Handshake: [username:password]\x00<version_byte>
        # Empty credentials, version byte
        handshake = b"\x00" + bytes([self._version])
        self._sock.sendall(handshake)
        
        # Read response (1 byte: 0x01 = success, 0x00 = failure)
        response = self._sock.recv(1)
        if response != b"\x01":
            error_msg = self._sock.recv(1024).decode("utf-8", errors="ignore")
            raise ConnectionError(f"Handshake failed: {error_msg}")
    
    def send_sync(self, query: str) -> bytes:
        """Send a sync query and wait for response."""
        if not self._sock:
            raise ConnectionError("Not connected")
        
        # Serialize string query
        query_bytes = query.encode("utf-8")
        # String serialization: type byte (-10 for string) + length (8 bytes) + data
        payload = struct.pack("<b", -10) + struct.pack("<q", len(query_bytes)) + query_bytes
        
        # Build header
        header = struct.pack(
            "<IBBBBQ",  # little-endian: u32, u8, u8, u8, u8, u64
            SERDE_PREFIX,
            self._version,
            0,  # flags
            0,  # endian (little)
            MSG_TYPE_SYNC,
            len(payload),
        )
        
        # Send
        self._sock.sendall(header + payload)
        
        # Read response header
        resp_header = self._recv_exact(HEADER_SIZE)
        prefix, version, flags, endian, msgtype, size = struct.unpack("<IBBBBQ", resp_header)
        
        if prefix != SERDE_PREFIX:
            raise ValueError(f"Invalid response prefix: {prefix:#x}")
        
        # Read response payload
        resp_payload = self._recv_exact(size)
        return resp_payload
    
    def _recv_exact(self, size: int) -> bytes:
        """Receive exactly size bytes."""
        data = b""
        while len(data) < size:
            chunk = self._sock.recv(size - len(data))
            if not chunk:
                raise ConnectionError("Connection closed")
            data += chunk
        return data
    
    def close(self) -> None:
        """Close the connection."""
        if self._sock:
            self._sock.close()
            self._sock = None


# Type mapping from manifest types to Rayforce types
TYPE_MAP = {
    "I64": "I64",
    "I32": "I32",
    "I16": "I16",
    "F64": "F64",
    "F32": "F32",
    "SYMBOL": "SYMBOL",
    "STRING": "C8",
    "DATE": "DATE",
    "TIME": "TIME",
    "TIMESTAMP": "TIMESTAMP",
    "BOOL": "B8",
    "B8": "B8",
}


class RayforceAdapter(Adapter):
    """RayforceDB adapter.
    
    Supports two modes:
    - IPC mode (use_ipc=True): Starts server, loads data once, queries via socket.
      This is FAIR - data stays in memory like DuckDB/Polars.
    - Subprocess mode (use_ipc=False): Spawns new process per query, reloads CSV.
      This is UNFAIR but works as fallback.
    """
    
    name = "rayforce"
    version = "0.1.0"
    embedded = False
    
    def __init__(
        self,
        binary_path: str | Path | None = None,
        use_ipc: bool | None = None,
        host: str | None = None,
        port: int | None = None,
    ):
        """Initialize Rayforce adapter.

        Args:
            binary_path: Path to rayforce binary (default: from config).
            use_ipc: If True, use IPC mode with server (default: True for fairness).
            host: IPC server host.
            port: IPC server port.
        """
        config = get_config()
        rf_config = config.rayforce

        self.binary_path = Path(binary_path or rf_config.get("binary", "rayforce"))
        # Subprocess mode is default (IPC mode requires manual server setup)
        # Note: Subprocess mode is still FAIR because timeit measures only query, not CSV loading
        self.use_ipc = use_ipc if use_ipc is not None else rf_config.get("use_ipc", False)
        self.host = host or rf_config.get("host", "127.0.0.1")
        self.port = port or rf_config.get("port", 5110)
        
        self._schema: dict[str, Any] = {}
        self._table_name: str = ""
        self._csv_paths: list[Path] = []
        self._tables: dict[str, Path] = {}
        self._tasks = self._build_task_registry()
        
        # IPC mode state
        self._server_proc: subprocess.Popen | None = None
        self._ipc_client: RayforceIPCClient | None = None
        self._data_loaded: bool = False
    
    def _build_task_registry(self) -> dict[str, callable]:
        """Build registry of task handlers."""
        return {
            # H2OAI Group By queries (Rayforce syntax)
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
            # Generic expression execution
            "eval": self._task_eval,
        }
    
    def setup(self, schema: dict[str, Any]) -> None:
        """Initialize Rayforce environment.
        
        In IPC mode: starts server process.
        In subprocess mode: verifies binary exists.
        """
        self._schema = schema
        self._table_name = schema.get("table_name", "t")
        
        # Verify rayforce binary exists
        try:
            result = subprocess.run(
                [str(self.binary_path), "--version"],
                capture_output=True,
                text=True,
                timeout=5,
            )
            if result.returncode == 0:
                self.version = result.stdout.strip() or "unknown"
        except (subprocess.SubprocessError, FileNotFoundError) as e:
            raise SetupError(f"Rayforce binary not found: {self.binary_path}. Error: {e}")
        
        if self.use_ipc:
            self._start_server()
    
    def _start_server(self) -> None:
        """Start rayforce server process for IPC mode."""
        import time as _time

        # Start server
        cmd = [str(self.binary_path), "-p", str(self.port)]

        self._server_proc = subprocess.Popen(
            cmd,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        
        # Wait for server to start
        _time.sleep(0.5)
        
        # Check if server started
        if self._server_proc.poll() is not None:
            stderr = self._server_proc.stderr.read().decode() if self._server_proc.stderr else ""
            raise SetupError(f"Failed to start rayforce server: {stderr}")
        
        # Connect client
        try:
            self._ipc_client = RayforceIPCClient(self.host, self.port)
            self._ipc_client.connect()
        except Exception as e:
            self._stop_server()
            raise SetupError(f"Failed to connect to rayforce server: {e}")
    
    def _stop_server(self) -> None:
        """Stop rayforce server process."""
        if self._ipc_client:
            try:
                self._ipc_client.close()
            except Exception:
                pass
            self._ipc_client = None
        
        if self._server_proc:
            self._server_proc.terminate()
            try:
                self._server_proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self._server_proc.kill()
            self._server_proc = None
        
        self._data_loaded = False
    
    def load_csv(self, csv_paths: list[Path], table_name: str) -> None:
        """Load CSV data.
        
        In IPC mode: sends load command to server (data stays in memory).
        In subprocess mode: stores paths for loading at query time.
        """
        self._csv_paths = csv_paths
        self._table_name = table_name
        if csv_paths:
            self._tables[table_name] = csv_paths[0]
        
        if self.use_ipc and self._ipc_client and not self._data_loaded:
            # Load data on server
            schema_str = self._build_schema_str()
            for tbl_name, tbl_path in self._tables.items():
                load_expr = f'(set {tbl_name} (read-csv {schema_str} "{tbl_path}"))'
                try:
                    self._ipc_client.send_sync(load_expr)
                except Exception as e:
                    raise SetupError(f"Failed to load CSV on server: {e}")
            self._data_loaded = True
    
    def run(self, task: str, params: dict[str, Any]) -> AdapterResult:
        """Execute a benchmark task."""
        handler = self._tasks.get(task)
        if handler is None:
            if "expr" in params:
                return self._execute_expr(params["expr"])
            raise TaskError(f"Unknown task: {task}")
        
        return handler(params)
    
    def close(self) -> None:
        """Clean up Rayforce resources."""
        if self.use_ipc:
            self._stop_server()
    
    def clear_cache(self) -> None:
        """Clear Rayforce caches for cold-run benchmarks."""
        # In IPC mode, we could potentially reload data
        # For now, this is a no-op
        pass
    
    def get_info(self) -> dict[str, Any]:
        """Return Rayforce-specific metadata."""
        info = super().get_info()
        info.update({
            "rayforce_version": self.version,
            "mode": "ipc" if self.use_ipc else "subprocess",
            "binary_path": str(self.binary_path),
        })
        return info
    
    def _build_schema_str(self) -> str:
        """Build Rayforce type schema string from manifest schema.
        
        Example: [SYMBOL SYMBOL SYMBOL I64 I64 I64 I64 I64 F64]
        """
        columns = self._schema.get("columns", [])
        types = []
        for col in columns:
            manifest_type = col.get("type", "STRING")
            rf_type = TYPE_MAP.get(manifest_type, "C8")
            types.append(rf_type)
        return "[" + " ".join(types) + "]"
    
    def _execute_expr(self, expr: str) -> AdapterResult:
        """Execute a Rayforce expression and return result metadata.
        
        Uses IPC mode if connected (fair - data in memory).
        Falls back to subprocess mode (unfair - reloads CSV).
        """
        if self.use_ipc and self._ipc_client:
            return self._execute_expr_ipc(expr)
        else:
            return self._execute_expr_subprocess(expr)
    
    def _execute_expr_ipc(self, expr: str) -> AdapterResult:
        """Execute expression via IPC (data already loaded on server)."""
        start_ns = time.perf_counter_ns()
        
        try:
            # Send timed query - server has data already loaded
            timed_query = f"(timeit {expr})"
            self._ipc_client.send_sync(timed_query)
            
            # Get timing result
            timing_query = "(println _result)"  # timeit stores in _result
            
            # Actually, let's do it differently - execute and get timing + count
            query = f"""(do
                (set _t (timeit {expr}))
                (set _r {expr})
                (list _t (count _r)))"""
            
            response = self._ipc_client.send_sync(query)
            end_ns = time.perf_counter_ns()
            
            # Parse response - expect [timing_ms, row_count]
            # Response is serialized - for now use external timing
            # TODO: Parse binary response to get actual timing
            
            # For now, use Python timing (still fair since data is in memory)
            execution_ns = end_ns - start_ns
            
            return AdapterResult(
                execution_time_ns=execution_ns,
                row_count=0,  # TODO: Parse from response
                checksum=None,
                query=expr,
            )
            
        except Exception as e:
            import traceback
            return AdapterResult(
                execution_time_ns=time.perf_counter_ns() - start_ns,
                row_count=0,
                success=False,
                error_message=f"{type(e).__name__}: {e}\n{traceback.format_exc()}",
                query=expr,
            )
    
    def _execute_expr_subprocess(self, expr: str) -> AdapterResult:
        """Execute expression via subprocess.
        
        Uses Rayforce's internal `timeit` for accurate query timing.
        CSV is loaded before timing starts.
        """
        import tempfile
        import os
        
        start_ns = time.perf_counter_ns()
        
        try:
            csv_path = str(self._csv_paths[0]) if self._csv_paths else ""
            schema_str = self._build_schema_str()
            
            # Build table loading statements
            load_statements = []
            if self._tables:
                for tbl_name, tbl_path in self._tables.items():
                    load_statements.append(f'(set {tbl_name} (read-csv {schema_str} "{tbl_path}"))')
            else:
                load_statements.append(f'(set {self._table_name} (read-csv {schema_str} "{csv_path}"))')
            
            load_script = "\n".join(load_statements)
            
            # Use timeit to get accurate timing from Rayforce itself (10 iterations)
            full_script = f'''{load_script}
(set _timing (timeit 10 {expr}))
(set _result {expr})
(set _count (count _result))
(print "TIMING_MS:")
(println _timing)
(print "ROW_COUNT:")
(println _count)
'''
            
            with tempfile.NamedTemporaryFile(mode='w', suffix='.rfl', delete=False) as f:
                f.write(full_script)
                script_path = f.name
            
            try:
                cmd = [str(self.binary_path), "-f", script_path]

                result = subprocess.run(
                    cmd,
                    capture_output=True,
                    text=True,
                    stdin=subprocess.DEVNULL,
                    timeout=300,
                )
                
                end_ns = time.perf_counter_ns()
                
                output = result.stdout.strip()
                stderr = result.stderr.strip()
                
                # Check for errors
                if result.returncode != 0:
                    error_msg = stderr or output or f"Exit code {result.returncode}"
                    return AdapterResult(
                        execution_time_ns=end_ns - start_ns,
                        row_count=0,
                        success=False,
                        error_message=error_msg,
                        query=expr,
                    )
                
                # Check for error patterns in output
                if "error" in output.lower() or "Error" in output or output.startswith("'"):
                    return AdapterResult(
                        execution_time_ns=end_ns - start_ns,
                        row_count=0,
                        success=False,
                        error_message=f"Rayforce error: {output}",
                        query=expr,
                    )
                
                # Parse timing and row count from output
                timing_ms = None
                row_count = 0
                
                for line in output.split('\n'):
                    if line.startswith('TIMING_MS:'):
                        try:
                            timing_ms = float(line.split(':')[1].strip())
                        except (ValueError, IndexError):
                            pass
                    elif line.startswith('ROW_COUNT:'):
                        try:
                            row_count = int(line.split(':')[1].strip())
                        except (ValueError, IndexError):
                            pass
                
                # Use Rayforce's timeit result (measures query execution only)
                # timeit returns total time for 10 iterations, so divide by 10
                if timing_ms is not None:
                    execution_ns = int(timing_ms * 1_000_000 / 10)
                else:
                    execution_ns = end_ns - start_ns  # Fallback to Python timing
                
                checksum = int(hashlib.md5(output.encode()).hexdigest()[:8], 16) if output else None
                
                return AdapterResult(
                    execution_time_ns=execution_ns,
                    row_count=row_count,
                    checksum=checksum,
                    query=expr,
                )
            finally:
                os.unlink(script_path)
            
        except subprocess.TimeoutExpired:
            return AdapterResult(
                execution_time_ns=time.perf_counter_ns() - start_ns,
                row_count=0,
                success=False,
                error_message=f"Execution timeout (>300s)",
                query=expr,
            )
        except Exception as e:
            import traceback
            return AdapterResult(
                execution_time_ns=time.perf_counter_ns() - start_ns,
                row_count=0,
                success=False,
                error_message=f"{type(e).__name__}: {e}\n{traceback.format_exc()}",
                query=expr,
            )
    
    # =========================================================================
    # H2OAI Group By Queries (Rayforce syntax)
    # From Rayforce docs: (select {v1: (sum v1) from: t by: id1})
    # =========================================================================
    
    def _task_groupby_q1(self, params: dict[str, Any]) -> AdapterResult:
        """Q1: sum(v1) by id1"""
        table = params.get("table", self._table_name)
        expr = f"(select {{v1: (sum v1) from: {table} by: id1}})"
        return self._execute_expr(expr)
    
    def _task_groupby_q2(self, params: dict[str, Any]) -> AdapterResult:
        """Q2: sum(v1) by id1, id2"""
        table = params.get("table", self._table_name)
        expr = f"(select {{v1: (sum v1) from: {table} by: {{id1: id1 id2: id2}}}})"
        return self._execute_expr(expr)
    
    def _task_groupby_q3(self, params: dict[str, Any]) -> AdapterResult:
        """Q3: sum(v1), avg(v3) by id3"""
        table = params.get("table", self._table_name)
        expr = f"(select {{v1: (sum v1) v3: (avg v3) from: {table} by: id3}})"
        return self._execute_expr(expr)
    
    def _task_groupby_q4(self, params: dict[str, Any]) -> AdapterResult:
        """Q4: avg(v1), avg(v2), avg(v3) by id4"""
        table = params.get("table", self._table_name)
        expr = f"(select {{v1: (avg v1) v2: (avg v2) v3: (avg v3) from: {table} by: id4}})"
        return self._execute_expr(expr)
    
    def _task_groupby_q5(self, params: dict[str, Any]) -> AdapterResult:
        """Q5: sum(v1), sum(v2), sum(v3) by id6"""
        table = params.get("table", self._table_name)
        expr = f"(select {{v1: (sum v1) v2: (sum v2) v3: (sum v3) from: {table} by: id6}})"
        return self._execute_expr(expr)
    
    def _task_groupby_q6(self, params: dict[str, Any]) -> AdapterResult:
        """Q6: max(v1) - min(v2) by id3"""
        table = params.get("table", self._table_name)
        expr = f"(select {{range_v1_v2: (- (max v1) (min v2)) from: {table} by: id3}})"
        return self._execute_expr(expr)
    
    def _task_groupby_q7(self, params: dict[str, Any]) -> AdapterResult:
        """Q7: sum(v3), count by id1-id6"""
        table = params.get("table", self._table_name)
        expr = f"""(select {{v3: (sum v3) cnt: (count v3) from: {table} by: {{id1: id1 id2: id2 id3: id3 id4: id4 id5: id5 id6: id6}}}})"""
        return self._execute_expr(expr)
    
    def _task_groupby_q8(self, params: dict[str, Any]) -> AdapterResult:
        """Q8: Range filter + aggregation: sum(v3) by id2 where v1 >= 3"""
        table = params.get("table", self._table_name)
        expr = f"(select {{v3: (sum v3) from: {table} where: (>= v1 3) by: id2}})"
        return self._execute_expr(expr)
    
    def _task_groupby_q9(self, params: dict[str, Any]) -> AdapterResult:
        """Q9: Compound filter + multi-agg: sum(v1,v2,v3) by id3 where v1>=2 AND v2<=8"""
        table = params.get("table", self._table_name)
        expr = f"(select {{v1: (sum v1) v2: (sum v2) v3: (sum v3) from: {table} where: (and (>= v1 2) (<= v2 8)) by: id3}})"
        return self._execute_expr(expr)
    
    def _task_groupby_q10(self, params: dict[str, Any]) -> AdapterResult:
        """Q10: Filter + group: sum(v1), sum(v2) by id1-id4 where v3>0"""
        table = params.get("table", self._table_name)
        expr = f"""(select {{v1: (sum v1) v2: (sum v2) from: {table} where: (> v3 0) by: {{id1: id1 id2: id2 id3: id3 id4: id4}}}})"""
        return self._execute_expr(expr)
    
    # =========================================================================
    # Join Queries (Rayforce syntax)
    # From Rayforce docs: (ij [id1 id2] x y), (lj [id1 id2] x y)
    # =========================================================================
    
    def _task_inner_join(self, params: dict[str, Any]) -> AdapterResult:
        """Inner join on id1, id2"""
        left_table = params.get("left_table", "x")
        right_table = params.get("right_table", "y")
        expr = f"(inner-join [id1 id2] {left_table} {right_table})"
        return self._execute_expr(expr)
    
    def _task_left_join(self, params: dict[str, Any]) -> AdapterResult:
        """Left join on id1, id2"""
        left_table = params.get("left_table", "x")
        right_table = params.get("right_table", "y")
        expr = f"(left-join [id1 id2] {left_table} {right_table})"
        return self._execute_expr(expr)

    # =========================================================================
    # Sort Queries (Rayforce syntax)
    # From Rayforce docs: (xasc [col] table) or (xdesc [col] table)
    # =========================================================================

    def _task_sort_single(self, params: dict[str, Any]) -> AdapterResult:
        """Sort by single column"""
        table = params.get("table", self._table_name)
        column = params.get("column", "id1")
        descending = params.get("descending", False)
        if descending:
            expr = f"(xdesc [{column}] {table})"
        else:
            expr = f"(xasc [{column}] {table})"
        return self._execute_expr(expr)

    def _task_sort_multi(self, params: dict[str, Any]) -> AdapterResult:
        """Sort by multiple columns"""
        table = params.get("table", self._table_name)
        columns = params.get("columns", ["id1", "id2"])
        # Rayforce multi-column sort: (xasc [col1 col2] table)
        cols_str = " ".join(columns)
        expr = f"(xasc [{cols_str}] {table})"
        return self._execute_expr(expr)

    # =========================================================================
    # Window Join Queries (Rayforce syntax)
    # From Rayforce docs: (window-join1 [Sym Ts] intervals trades quotes {aggs})
    # =========================================================================

    def _task_window_join(self, params: dict[str, Any]) -> AdapterResult:
        """Window join (wj1) - join within time window with aggregations"""
        trades_table = params.get("trades_table", "trades")
        quotes_table = params.get("quotes_table", "quotes")
        keys = params.get("keys", ["Sym", "Ts"])
        window_ms = params.get("window_ms", 10000)  # +/- 10 seconds default

        keys_str = " ".join(keys)
        # Build intervals from trades timestamp
        expr = f"""(do
            (set _intervals (map-left + [-{window_ms} {window_ms}] (at {trades_table} 'Ts)))
            (window-join1 [{keys_str}] _intervals {trades_table} {quotes_table} {{Bid: (min Bid) Ask: (max Ask)}}))"""
        return self._execute_expr(expr)

    def _task_eval(self, params: dict[str, Any]) -> AdapterResult:
        """Execute arbitrary Rayforce expression."""
        expr = params.get("expr")
        if not expr:
            return AdapterResult(
                execution_time_ns=0,
                row_count=0,
                success=False,
                error_message="No expression provided",
            )
        return self._execute_expr(expr)


# =============================================================================
# TODO: Native Bindings Implementation
# =============================================================================
#
# For optimal embedded benchmarking, implement native Python bindings:
#
# import ctypes
# from ctypes import c_char_p, c_int, c_void_p, POINTER
#
# class RayforceNativeAdapter(Adapter):
#     """Native Rayforce adapter using ctypes bindings."""
#     
#     name = "rayforce"
#     embedded = True
#     
#     def __init__(self, lib_path: str = "librayforce.so"):
#         self._lib = ctypes.CDLL(lib_path)
#         
#         # Define function signatures
#         self._lib.ray_init.argtypes = []
#         self._lib.ray_init.restype = c_int
#         
#         self._lib.eval_str.argtypes = [c_char_p]
#         self._lib.eval_str.restype = c_void_p
#         
#         self._lib.drop_obj.argtypes = [c_void_p]
#         self._lib.drop_obj.restype = None
#         
#         self._lib.ray_clean.argtypes = []
#         self._lib.ray_clean.restype = None
#     
#     def setup(self, schema):
#         self._lib.ray_init()
#     
#     def run(self, task, params):
#         start_ns = time.perf_counter_ns()
#         expr = self._build_expr(task, params)
#         result = self._lib.eval_str(expr.encode())
#         end_ns = time.perf_counter_ns()
#         
#         # Extract row count from result object
#         # ...
#         
#         self._lib.drop_obj(result)
#         return AdapterResult(execution_time_ns=end_ns - start_ns, ...)
#     
#     def close(self):
#         self._lib.ray_clean()
