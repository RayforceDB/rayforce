"""Compare the same local append/amend workload in fresh runtime processes."""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess

HERE = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--rayforce", required=True)
parser.add_argument("--q", default=shutil.which("q"))
parser.add_argument("--peachq", default=shutil.which("peachq"))
parser.add_argument("--amber", default=shutil.which("amberx"))
parser.add_argument(
    "--amber-home", default=os.environ.get("AMBER_HOME", "/usr/share/amber")
)
parser.add_argument("--processes", type=int, default=3)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
if args.processes < 1:
    parser.error("--processes must be positive")
commands = {"rayforce": [args.rayforce, "-c", "1", "-f", str(HERE / "local.rfl")]}
for name in ("q", "peachq", "amber"):
    binary = getattr(args, name)
    if binary:
        commands[name] = [binary, str(HERE / "local.q")]
        if name != "amber":
            commands[name] += ["-q", "-s", "0"]
env = dict(
    os.environ, AMBER_THREADS="1", AMBER_HOME=str(Path(args.amber_home).expanduser())
)
env.pop("RAYFORCE_CORES", None)
records = []
for trial in range(args.processes):
    order = list(commands)
    offset = trial % len(order)
    for engine in order[offset:] + order[:offset]:
        run = subprocess.run(
            commands[engine],
            capture_output=True,
            text=True,
            check=True,
            timeout=60,
            env=env,
        )
        samples = []
        for line in run.stdout.splitlines():
            match = re.fullmatch(
                r'"?BENCH\|local\|(1000|2000)\|([0-9.eE+-]+)\|(\d+)\|(\d+)"?', line
            )
            if not match:
                continue
            steps, ms, rows, checksum = match.groups()
            steps, rows, checksum = int(steps), int(rows), int(checksum)
            if rows != 500 * steps or checksum != 10 * steps:
                raise RuntimeError(f"{engine}: wrong result: {line}")
            samples.append(
                dict(steps=steps, ms=float(ms), rows=rows, checksum=checksum)
            )
        if len(samples) != 10:
            raise RuntimeError(
                f"{engine}: expected 10 samples: {run.stdout} {run.stderr}"
            )
        records.append(
            dict(engine=engine, trial=trial, command=commands[engine], samples=samples)
        )
args.output.write_text(json.dumps(records, indent=2) + "\n")
for engine in commands:
    medians = {
        steps: statistics.median(
            sample["ms"]
            for record in records
            if record["engine"] == engine
            for sample in record["samples"]
            if sample["steps"] == steps
        )
        for steps in (1000, 2000)
    }
    print(engine, medians)
