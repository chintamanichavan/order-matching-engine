"""Print the 'all calls' latency of each saved run, one row per file."""
import glob
import json
import sys

print(f"{'run':28} {'p50':>6} {'p99':>6} {'p99.9':>7} {'p99.99':>7} {'max':>8} {'allocs':>7}")
for path in sorted(glob.glob(sys.argv[1] if len(sys.argv) > 1 else "bench/results/*.json")):
    d = json.load(open(path))
    a = d["latency_ns"]["all"]
    name = path.rsplit("/", 1)[-1].removesuffix(".json")
    print(f"{name:28} {a['p50']:>6} {a['p99']:>6} {a['p99_9']:>7} {a['p99_99']:>7} {a['max']:>8} {d['allocations_per_call']:>7}")
