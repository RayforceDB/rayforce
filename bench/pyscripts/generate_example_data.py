#!/usr/bin/env python3
"""
Generate H2OAI db-benchmark compatible datasets.

File naming convention: G1_{N}_{K}_{NAs}_{seed}.csv
- N: number of rows (1e7 = 10M, 1e8 = 100M, 1e9 = 1B)
- K: number of unique groups (1e2 = 100)
- NAs: percentage of NA values (0 = none)
- seed: random seed
"""

import csv
import random
import sys
from pathlib import Path


def generate_h2oai_groupby(
    output_path: Path,
    n_rows: int = 10_000_000,
    k: int = 100,  # number of unique groups for id columns
    na_pct: int = 0,  # percentage of NA values
    seed: int = 0,
) -> None:
    """Generate H2OAI Group By compatible dataset.
    
    Exact H2OAI db-benchmark format:
        id1: string "id%03d" with K unique values (low cardinality)
        id2: string "id%03d" with K unique values (low cardinality)  
        id3: string "id%09d" with N/K unique values (high cardinality)
        id4: integer 1..K (low cardinality)
        id5: integer 1..K (low cardinality)
        id6: integer 1..N/K (high cardinality)
        v1: integer 1..5
        v2: integer 1..15
        v3: float uniform [0, 100)
    """
    random.seed(seed)
    
    # H2OAI uses K for low cardinality, N/K for high cardinality
    n_high = max(n_rows // k, k)  # High cardinality group count
    
    # Pre-generate values for efficiency
    id1_values = [f"id{i:03d}" for i in range(1, k + 1)]
    id2_values = [f"id{i:03d}" for i in range(1, k + 1)]
    # id3 has high cardinality - use 9-digit format
    id3_values = [f"id{i:09d}" for i in range(1, n_high + 1)]
    id4_values = list(range(1, k + 1))
    id5_values = list(range(1, k + 1))
    id6_values = list(range(1, n_high + 1))
    
    output_path.parent.mkdir(parents=True, exist_ok=True)
    
    print(f"Generating {n_rows:,} rows...")
    print(f"  Low cardinality (K): {k}")
    print(f"  High cardinality (N/K): {n_high}")
    
    with open(output_path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["id1", "id2", "id3", "id4", "id5", "id6", "v1", "v2", "v3"])
        
        # Generate in batches for progress reporting
        batch_size = 1_000_000
        rows_written = 0
        
        while rows_written < n_rows:
            batch_end = min(rows_written + batch_size, n_rows)
            for _ in range(batch_end - rows_written):
                # NA handling (if na_pct > 0)
                if na_pct > 0 and random.randint(1, 100) <= na_pct:
                    # Insert NA for some columns
                    row = [
                        "" if random.random() < 0.5 else random.choice(id1_values),
                        "" if random.random() < 0.5 else random.choice(id2_values),
                        random.choice(id3_values),
                        random.choice(id4_values),
                        random.choice(id5_values),
                        random.choice(id6_values),
                        random.randint(1, 5),
                        random.randint(1, 15),
                        round(random.uniform(0, 100), 6),
                    ]
                else:
                    row = [
                        random.choice(id1_values),
                        random.choice(id2_values),
                        random.choice(id3_values),
                        random.choice(id4_values),
                        random.choice(id5_values),
                        random.choice(id6_values),
                        random.randint(1, 5),
                        random.randint(1, 15),
                        round(random.uniform(0, 100), 6),
                    ]
                writer.writerow(row)
            
            rows_written = batch_end
            if n_rows >= 1_000_000:
                pct = (rows_written / n_rows) * 100
                print(f"  Progress: {rows_written:,} / {n_rows:,} ({pct:.0f}%)")
    
    print(f"Generated {n_rows:,} rows to {output_path}")


def generate_h2oai_join(
    output_dir: Path,
    n_rows: int = 1000,
    seed: int = 42,
) -> None:
    """Generate H2OAI Join compatible dataset (two tables).
    
    Creates:
        - x table (left): J1_{n_rows}_NA_0_0.csv
        - y table (right): J1_{n_rows}_{n_rows}_0_0.csv
    """
    random.seed(seed)
    
    output_dir.mkdir(parents=True, exist_ok=True)
    
    # Generate x table (left)
    x_path = output_dir / f"J1_{n_rows}_NA_0_0.csv"
    with open(x_path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["id1", "id2", "id3", "id4", "id5", "id6", "v1"])
        
        for i in range(n_rows):
            row = [
                i % 100 + 1,  # id1
                i % 100 + 1,  # id2
                i % 100 + 1,  # id3
                f"id{i % 100 + 1:03d}",  # id4
                f"id{i % 100 + 1:03d}",  # id5
                f"id{i % 100 + 1:03d}",  # id6
                round(random.uniform(0, 100), 6),  # v1
            ]
            writer.writerow(row)
    
    print(f"Generated {n_rows} rows to {x_path}")
    
    # Generate y table (right)
    y_path = output_dir / f"J1_{n_rows}_{n_rows}_0_0.csv"
    with open(y_path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["id1", "id2", "id3", "id4", "id5", "id6", "v2"])
        
        for i in range(n_rows):
            row = [
                i % 100 + 1,  # id1
                i % 100 + 1,  # id2
                i % 100 + 1,  # id3
                f"id{i % 100 + 1:03d}",  # id4
                f"id{i % 100 + 1:03d}",  # id5
                f"id{i % 100 + 1:03d}",  # id6
                round(random.uniform(0, 100), 6),  # v2
            ]
            writer.writerow(row)
    
    print(f"Generated {n_rows} rows to {y_path}")


def n_to_scientific(n: int) -> str:
    """Convert number to H2OAI scientific notation (1e7, 1e2, etc.)."""
    if n >= 1_000_000_000:
        return "1e9"
    elif n >= 100_000_000:
        return "1e8"
    elif n >= 10_000_000:
        return "1e7"
    elif n >= 1_000_000:
        return "1e6"
    elif n >= 100_000:
        return "1e5"
    elif n >= 10_000:
        return "1e4"
    elif n >= 1_000:
        return "1e3"
    elif n >= 100:
        return "1e2"
    elif n >= 10:
        return "1e1"
    else:
        return str(n)


def main():
    import argparse
    import json
    
    parser = argparse.ArgumentParser(
        description="Generate H2OAI db-benchmark compatible datasets",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Generate 10M row dataset (H2OAI standard)
  python generate_example_data.py --rows 1e7 --k 1e2

  # Generate 1B row dataset
  python generate_example_data.py --rows 1e9 --k 1e2
  
  # Generate with 5% NA values
  python generate_example_data.py --rows 1e7 --k 1e2 --na 5
"""
    )
    parser.add_argument(
        "--rows", "-n",
        type=str,
        default="1e7",
        help="Number of rows: 1e7 (10M), 1e8 (100M), 1e9 (1B), or exact number (default: 1e7)"
    )
    parser.add_argument(
        "--k", "-K",
        type=str, 
        default="1e2",
        help="Number of unique groups K: 1e2 (100), 1e1 (10), etc. (default: 1e2)"
    )
    parser.add_argument(
        "--na",
        type=int,
        default=0,
        help="Percentage of NA values 0-100 (default: 0)"
    )
    parser.add_argument(
        "--seed",
        type=int,
        default=0,
        help="Random seed (default: 0)"
    )
    parser.add_argument(
        "--type", "-t",
        choices=["groupby", "join", "all"],
        default="groupby",
        help="Dataset type (default: groupby)"
    )
    parser.add_argument(
        "--output", "-o",
        type=Path,
        default=None,
        help="Output directory (default: datasets/G1_<N>_<K>_<NA>_<seed>/)"
    )
    args = parser.parse_args()
    
    # Parse rows (support scientific notation)
    def parse_sci(s: str) -> int:
        s = s.lower().strip()
        if s.startswith("1e"):
            return int(10 ** int(s[2:]))
        return int(float(s))
    
    n_rows = parse_sci(args.rows)
    k = parse_sci(args.k)
    na_pct = args.na
    seed = args.seed
    
    # Generate H2OAI-style filename parts
    n_str = n_to_scientific(n_rows)
    k_str = n_to_scientific(k)
    
    project_root = Path(__file__).parent.parent
    datasets_dir = project_root / "datasets"
    
    if args.type in ("groupby", "all"):
        # H2OAI naming: G1_1e7_1e2_0_0
        dataset_name = f"G1_{n_str}_{k_str}_{na_pct}_{seed}"
        output_dir = args.output or datasets_dir / dataset_name
        output_dir.mkdir(parents=True, exist_ok=True)
        
        # Actual data file uses exact naming: G1_1e7_1e2_0_0.csv
        csv_filename = f"{dataset_name}.csv"
        
        print(f"\n=== Generating H2OAI Group By Dataset ===")
        print(f"  Name: {dataset_name}")
        print(f"  Rows (N): {n_rows:,}")
        print(f"  Groups (K): {k}")
        print(f"  NA %: {na_pct}")
        print(f"  Seed: {seed}")
        print()
        
        generate_h2oai_groupby(
            output_dir / csv_filename,
            n_rows=n_rows,
            k=k,
            na_pct=na_pct,
            seed=seed,
        )
        
        # Generate manifest
        manifest = {
            "name": dataset_name,
            "description": f"H2OAI Group By benchmark: {n_rows:,} rows, K={k}, {na_pct}% NA",
            "format": "h2oai",
            "table_name": "t",
            "row_count": n_rows,
            "k": k,
            "na_pct": na_pct,
            "seed": seed,
            "columns": [
                {"name": "id1", "type": "SYMBOL", "cardinality": "low", "groups": k},
                {"name": "id2", "type": "SYMBOL", "cardinality": "low", "groups": k},
                {"name": "id3", "type": "SYMBOL", "cardinality": "high", "groups": n_rows // k},
                {"name": "id4", "type": "I64", "cardinality": "low", "groups": k},
                {"name": "id5", "type": "I64", "cardinality": "low", "groups": k},
                {"name": "id6", "type": "I64", "cardinality": "high", "groups": n_rows // k},
                {"name": "v1", "type": "I64", "range": [1, 5]},
                {"name": "v2", "type": "I64", "range": [1, 15]},
                {"name": "v3", "type": "F64", "range": [0, 100]},
            ],
            "files": [csv_filename],
        }
        with open(output_dir / "manifest.json", "w") as f:
            json.dump(manifest, f, indent=2)
        
        print(f"\n✓ Dataset generated: {output_dir}")
        print(f"  CSV: {csv_filename}")
        print(f"  Manifest: manifest.json")
    
    if args.type in ("join", "all"):
        output_dir = args.output or datasets_dir / f"J1_{n_str}"
        output_dir.mkdir(parents=True, exist_ok=True)
        
        print(f"\n=== Generating H2OAI Join Dataset ===")
        print(f"  Rows: {n_rows:,}")
        print()
        
        generate_h2oai_join(
            output_dir,
            n_rows=n_rows,
            seed=seed,
        )
        
        # Generate manifest
        manifest = {
            "name": f"J1_{n_str}",
            "description": f"H2OAI Join benchmark: {n_rows:,} rows",
            "format": "h2oai",
            "row_count": n_rows,
            "seed": seed,
            "columns": [
                {"name": "id1", "type": "I64"},
                {"name": "id2", "type": "I64"},
                {"name": "id3", "type": "I64"},
                {"name": "id4", "type": "SYMBOL"},
                {"name": "id5", "type": "SYMBOL"},
                {"name": "id6", "type": "SYMBOL"},
                {"name": "v1", "type": "F64"},
            ],
            "tables": {
                "x": f"J1_{n_rows}_NA_0_0.csv",
                "y": f"J1_{n_rows}_{n_rows}_0_0.csv",
            },
        }
        with open(output_dir / "manifest.json", "w") as f:
            json.dump(manifest, f, indent=2)
        
        print(f"\n✓ Dataset generated: {output_dir}")
    
    print(f"\n✓ Dataset generation complete")


if __name__ == "__main__":
    main()
