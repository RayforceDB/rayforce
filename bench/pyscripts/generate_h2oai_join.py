#!/usr/bin/env python3
"""
Generate H2OAI join benchmark dataset.

Creates two CSV files:
- J1_1e7_NA_0_0.csv (x table): 10M rows
- J1_1e7_1e7_0_0.csv (y table): 10M rows

Based on: https://h2oai.github.io/db-benchmark
"""

import csv
import random
from pathlib import Path


def generate_h2oai_join_data(output_dir: Path, n: int = 10_000_000, k: int = 100, seed: int = 42):
    """Generate H2OAI join benchmark datasets."""

    random.seed(seed)
    output_dir.mkdir(parents=True, exist_ok=True)

    # Generate x table (J1_1e7_NA_0_0.csv)
    print(f"Generating x table ({n:,} rows)...")
    x_path = output_dir / "J1_1e7_NA_0_0.csv"

    with open(x_path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["id1", "id2", "id3", "id4", "id5", "id6", "v1"])

        for i in range(n):
            id1 = random.randint(1, k)
            id2 = random.randint(1, k)
            id3 = random.randint(1, n // k)  # high cardinality
            id4 = f"id{random.randint(1, k)}"
            id5 = f"id{random.randint(1, k)}"
            id6 = f"id{random.randint(1, n // k)}"
            v1 = round(random.uniform(0, 100), 6)
            writer.writerow([id1, id2, id3, id4, id5, id6, v1])

            if (i + 1) % 1_000_000 == 0:
                print(f"  {i + 1:,} rows...")

    print(f"  Written to {x_path}")

    # Generate y table (J1_1e7_1e7_0_0.csv)
    print(f"Generating y table ({n:,} rows)...")
    y_path = output_dir / "J1_1e7_1e7_0_0.csv"

    random.seed(seed + 1)  # Different seed for y

    with open(y_path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["id1", "id2", "id3", "id4", "id5", "id6", "v2"])

        for i in range(n):
            id1 = random.randint(1, k)
            id2 = random.randint(1, k)
            id3 = random.randint(1, n // k)
            id4 = f"id{random.randint(1, k)}"
            id5 = f"id{random.randint(1, k)}"
            id6 = f"id{random.randint(1, n // k)}"
            v2 = round(random.uniform(0, 100), 6)
            writer.writerow([id1, id2, id3, id4, id5, id6, v2])

            if (i + 1) % 1_000_000 == 0:
                print(f"  {i + 1:,} rows...")

    print(f"  Written to {y_path}")
    print("Done!")


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description="Generate H2OAI join benchmark data")
    parser.add_argument(
        "-n", "--rows",
        type=int,
        default=10_000_000,
        help="Number of rows per table"
    )
    parser.add_argument(
        "-k", "--cardinality",
        type=int,
        default=100,
        help="Low cardinality groups"
    )
    parser.add_argument(
        "-o", "--output",
        type=Path,
        default=Path("datasets/h2oai_join_1e7"),
        help="Output directory"
    )
    parser.add_argument(
        "-s", "--seed",
        type=int,
        default=42,
        help="Random seed"
    )

    args = parser.parse_args()
    generate_h2oai_join_data(args.output, args.rows, args.cardinality, args.seed)
