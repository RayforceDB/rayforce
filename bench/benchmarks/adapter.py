"""
Base adapter interface for database benchmarking.

All database adapters must implement this interface. The design prioritizes:
- Measuring only database execution time (not Python overhead)
- Returning minimal metadata instead of full result sets
- Supporting both embedded and server databases
"""

from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


@dataclass
class AdapterResult:
    """Result metadata from a benchmark task execution.
    
    Adapters return this instead of full result sets to avoid
    measuring data serialization and transfer costs.
    """
    # Timing (measured by adapter, not runner)
    execution_time_ns: int  # Nanoseconds for precision
    
    # Validation data (to verify correctness)
    row_count: int
    checksum: int | None = None  # Optional: hash of result for verification
    
    # Query/expression that was executed
    query: str | None = None  # The actual query/expression run
    
    # Optional metadata
    bytes_processed: int | None = None
    cpu_time_ns: int | None = None
    peak_memory_bytes: int | None = None
    
    # Error handling
    success: bool = True
    error_message: str | None = None
    
    # Adapter-specific metadata
    extra: dict[str, Any] = field(default_factory=dict)

    @property
    def execution_time_ms(self) -> float:
        """Execution time in milliseconds."""
        return self.execution_time_ns / 1_000_000
    
    @property
    def execution_time_s(self) -> float:
        """Execution time in seconds."""
        return self.execution_time_ns / 1_000_000_000


class Adapter(ABC):
    """Abstract base class for database adapters.
    
    Lifecycle:
        1. __init__() - Create adapter instance
        2. setup(schema) - Initialize database with schema
        3. load_csv(paths) - Load data from CSV files
        4. run(task, params) - Execute benchmark tasks (called many times)
        5. close() - Cleanup resources
    
    Subclasses should:
        - Use native/embedded APIs when available
        - Measure only database execution time
        - Return row counts and checksums, not full result sets
    """
    
    # Adapter identification
    name: str = "base"
    version: str = "0.0.0"
    embedded: bool = True  # True for in-process, False for client/server
    
    @abstractmethod
    def setup(self, schema: dict[str, Any]) -> None:
        """Initialize the database with the given schema.
        
        Args:
            schema: Column definitions and table metadata from manifest.
                    Format: {"columns": [{"name": "col1", "type": "I64"}, ...],
                             "table_name": "benchmark_table"}
        
        Called once before load_csv().
        """
        pass
    
    @abstractmethod
    def load_csv(self, csv_paths: list[Path], table_name: str) -> None:
        """Load data from CSV files into the database.
        
        Args:
            csv_paths: List of CSV file paths to load.
            table_name: Target table name.
        
        Called once after setup(), before any run() calls.
        For partitioned datasets, csv_paths may contain multiple files.
        """
        pass
    
    @abstractmethod
    def run(self, task: str, params: dict[str, Any]) -> AdapterResult:
        """Execute a benchmark task and return metadata.
        
        Args:
            task: Task identifier (e.g., "groupby_q1", "inner_join").
            params: Task-specific parameters.
        
        Returns:
            AdapterResult with timing and validation metadata.
        
        Important:
            - Time only the database operation, not setup/serialization
            - Return row count for validation, not full result set
            - Use adapter's native timing if available (more accurate)
        """
        pass
    
    @abstractmethod
    def close(self) -> None:
        """Clean up resources and close connections.
        
        Called once after all run() calls complete.
        """
        pass
    
    def clear_cache(self) -> None:
        """Optional: Clear database caches for cold-run benchmarks.
        
        Default implementation does nothing. Override if the database
        supports cache clearing.
        """
        pass
    
    def get_info(self) -> dict[str, Any]:
        """Return adapter metadata for reproducibility.
        
        Override to add database-specific version info, settings, etc.
        """
        return {
            "adapter": self.name,
            "adapter_version": self.version,
            "embedded": self.embedded,
        }


class AdapterError(Exception):
    """Base exception for adapter errors."""
    pass


class SetupError(AdapterError):
    """Error during adapter setup or data loading."""
    pass


class TaskError(AdapterError):
    """Error during task execution."""
    pass
