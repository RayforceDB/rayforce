"""
Configuration management for the benchmark framework.

Supports:
- config.yaml (default configuration)
- config.local.yaml (local overrides, gitignored)
- Environment variables (highest priority)
"""

import os
from pathlib import Path
from typing import Any

import yaml


# Environment variable mappings
ENV_MAPPINGS = {
    "RAYFORCE_BINARY": ("rayforce", "binary"),
    "RAYFORCE_USE_IPC": ("rayforce", "use_ipc"),
    "RAYFORCE_HOST": ("rayforce", "host"),
    "RAYFORCE_PORT": ("rayforce", "port"),
    "KDB_BINARY": ("kdb", "binary"),
    "KDB_USE_IPC": ("kdb", "use_ipc"),
    "KDB_HOST": ("kdb", "host"),
    "KDB_PORT": ("kdb", "port"),
    "DUCKDB_MEMORY_LIMIT": ("duckdb", "memory_limit"),
    "QUESTDB_HOST": ("questdb", "host"),
    "QUESTDB_PORT": ("questdb", "port"),
    "QUESTDB_DATABASE": ("questdb", "database"),
    "QUESTDB_USER": ("questdb", "user"),
    "QUESTDB_PASSWORD": ("questdb", "password"),
    "BENCH_REPORTS_DIR": ("general", "reports_dir"),
    "BENCH_DATASETS_DIR": ("general", "datasets_dir"),
    "BENCH_VERBOSE": ("general", "verbose"),
}


class Config:
    """Configuration container with environment variable support."""
    
    def __init__(self, config_dir: Path | None = None):
        """Load configuration from files and environment.
        
        Args:
            config_dir: Directory containing config files (default: project root)
        """
        if config_dir is None:
            # Default to project root (parent of benchmarks/)
            config_dir = Path(__file__).parent.parent
        
        self._config_dir = Path(config_dir)
        self._data: dict[str, Any] = {}
        self._load()
    
    def _load(self) -> None:
        """Load configuration from files and environment."""
        # Load default config
        default_path = self._config_dir / "config.yaml"
        if default_path.exists():
            with open(default_path) as f:
                self._data = yaml.safe_load(f) or {}
        
        # Load local overrides (gitignored)
        local_path = self._config_dir / "config.local.yaml"
        if local_path.exists():
            with open(local_path) as f:
                local_config = yaml.safe_load(f) or {}
                self._deep_merge(self._data, local_config)
        
        # Apply environment variable overrides
        self._apply_env_overrides()
    
    def _deep_merge(self, base: dict, override: dict) -> None:
        """Recursively merge override into base dict."""
        for key, value in override.items():
            if key in base and isinstance(base[key], dict) and isinstance(value, dict):
                self._deep_merge(base[key], value)
            else:
                base[key] = value
    
    def _apply_env_overrides(self) -> None:
        """Apply environment variable overrides."""
        for env_var, (section, key) in ENV_MAPPINGS.items():
            value = os.environ.get(env_var)
            if value is not None:
                # Ensure section exists
                if section not in self._data:
                    self._data[section] = {}

                # Convert value types
                if key == "port":
                    value = int(value) if value else None
                elif key in ("use_ipc", "verbose"):
                    value = value.lower() in ("true", "1", "yes")
                elif value == "":
                    value = None

                self._data[section][key] = value
    
    def get(self, section: str, key: str, default: Any = None) -> Any:
        """Get a configuration value.
        
        Args:
            section: Config section (e.g., "rayforce", "kdb")
            key: Config key within section
            default: Default value if not found
        
        Returns:
            Configuration value or default
        """
        return self._data.get(section, {}).get(key, default)
    
    def get_section(self, section: str) -> dict[str, Any]:
        """Get an entire configuration section.
        
        Args:
            section: Config section name
        
        Returns:
            Section dict or empty dict
        """
        return self._data.get(section, {})
    
    @property
    def rayforce(self) -> dict[str, Any]:
        """Rayforce configuration section."""
        return self.get_section("rayforce")
    
    @property
    def kdb(self) -> dict[str, Any]:
        """KDB configuration section."""
        return self.get_section("kdb")
    
    @property
    def duckdb(self) -> dict[str, Any]:
        """DuckDB configuration section."""
        return self.get_section("duckdb")
    
    @property
    def polars(self) -> dict[str, Any]:
        """Polars configuration section."""
        return self.get_section("polars")

    @property
    def questdb(self) -> dict[str, Any]:
        """QuestDB configuration section."""
        return self.get_section("questdb")

    @property
    def general(self) -> dict[str, Any]:
        """General configuration section."""
        return self.get_section("general")


# Global config instance (lazy loaded)
_config: Config | None = None


def get_config(config_dir: Path | None = None) -> Config:
    """Get the global configuration instance.
    
    Args:
        config_dir: Override config directory (only used on first call)
    
    Returns:
        Config instance
    """
    global _config
    if _config is None:
        _config = Config(config_dir)
    return _config


def reload_config(config_dir: Path | None = None) -> Config:
    """Reload configuration from files.
    
    Args:
        config_dir: Override config directory
    
    Returns:
        New Config instance
    """
    global _config
    _config = Config(config_dir)
    return _config
