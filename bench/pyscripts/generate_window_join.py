#!/usr/bin/env python3
"""
Generate window join benchmark dataset.

Creates two CSV files:
- trades.csv: 10M rows with Sym, Ts (TIME), Price
- quotes.csv: 20M rows with Sym, Ts (TIME), Bid, Ask

Based on the Rayforce window join benchmark from the docs.
"""

import csv
import os
from pathlib import Path


def generate_window_join_data(output_dir: Path, n_trades: int = 10_000_000):
    """Generate trades and quotes CSV files for window join benchmark."""

    output_dir.mkdir(parents=True, exist_ok=True)
    n_quotes = 2 * n_trades

    # Generate trades
    print(f"Generating {n_trades:,} trades...")
    trades_path = output_dir / "trades.csv"

    with open(trades_path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["Sym", "Ts", "Price"])

        # Pattern: 99 AAPL, 1 MSFT repeating
        for i in range(n_trades):
            sym = "AAPL" if (i % 100) < 99 else "MSFT"
            # Timestamps: 09:00:00 + (3 * i) / 10 milliseconds
            ts_ms = 9 * 3600 * 1000 + (3 * i) // 10
            ts_str = format_time(ts_ms)
            price = 10 + i
            writer.writerow([sym, ts_str, price])

    print(f"  Written to {trades_path}")

    # Generate quotes
    print(f"Generating {n_quotes:,} quotes...")
    quotes_path = output_dir / "quotes.csv"

    with open(quotes_path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["Sym", "Ts", "Bid", "Ask"])

        # Pattern: AAPL, AAPL, AAPL, MSFT, MSFT, GOOG repeating
        sym_pattern = ["AAPL", "AAPL", "AAPL", "MSFT", "MSFT", "GOOG"]
        for i in range(n_quotes):
            sym = sym_pattern[i % 6]
            # Timestamps: 09:00:00 + (2 * i) / 10 milliseconds
            ts_ms = 9 * 3600 * 1000 + (2 * i) // 10
            ts_str = format_time(ts_ms)
            bid = 8 + i // 2
            ask = 12 + i // 2
            writer.writerow([sym, ts_str, bid, ask])

    print(f"  Written to {quotes_path}")
    print("Done!")


def format_time(ms: int) -> str:
    """Format milliseconds since midnight as HH:MM:SS.mmm"""
    hours = ms // 3600000
    ms %= 3600000
    minutes = ms // 60000
    ms %= 60000
    seconds = ms // 1000
    millis = ms % 1000
    return f"{hours:02d}:{minutes:02d}:{seconds:02d}.{millis:03d}"


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description="Generate window join benchmark data")
    parser.add_argument(
        "-n", "--rows",
        type=int,
        default=10_000_000,
        help="Number of trade rows (quotes = 2x this)"
    )
    parser.add_argument(
        "-o", "--output",
        type=Path,
        default=Path("datasets/window_join_10m"),
        help="Output directory"
    )

    args = parser.parse_args()
    generate_window_join_data(args.output, args.rows)
