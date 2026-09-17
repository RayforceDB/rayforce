"""Shared comparison and profile parsing for synthetic scaling benchmarks."""
import csv
import math
import re


def profile_phases(log):
    return [dict(label=label, ms=float(value)) for label, value in
            re.findall(r"✶\s+(.+): ([0-9.]+) ms", log)]


def compare_csv(reference, current, schema, key_names=("k",)):
    """Exact keys/types/nulls; round-trip floats with explicit tolerances."""
    with schema.open(newline="") as stream:
        kinds = {r["column"]: r["kind"] for r in csv.DictReader(stream)}
    with reference.open(newline="") as a, current.open(newline="") as b:
        left_reader, right_reader = csv.DictReader(a), csv.DictReader(b)
        if left_reader.fieldnames != right_reader.fieldnames or left_reader.fieldnames != list(kinds):
            raise RuntimeError("Result columns differ")
        keys = [name for name in key_names if name in kinds]
        key = lambda row: tuple(row[name] for name in keys)
        # Canonicalize independently of engine sort/null tie behavior.
        left, right = sorted(left_reader, key=key), sorted(right_reader, key=key)
        if len(left) != len(right):
            raise RuntimeError("Result row counts differ")
        for row_number, (row, other) in enumerate(zip(left, right)):
            if row_number and (key(row) == key(left[row_number - 1]) or key(other) == key(right[row_number - 1])):
                raise RuntimeError("Duplicate output group key")
            for name, value in row.items():
                got = other[name]
                if value == got:
                    continue
                if name in key_names or kinds[name] not in {"F32", "F64"} or not value or not got:
                    raise RuntimeError(f"Result differs at row {row_number}, column {name}: {value!r} / {got!r}")
                if not math.isclose(float(value), float(got), rel_tol=1e-9, abs_tol=1e-8):
                    raise RuntimeError(f"Float differs at row {row_number}, column {name}: {value} / {got}")
