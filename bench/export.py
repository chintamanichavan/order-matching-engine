"""Write the latency study's results as one small JSON file for the portfolio page.

    python3 bench/export.py > ../portfolio/src/data/latency-study.json
"""
import glob
import json

STAGES = [
    ("0-baseline", "Before", "The engine as it was."),
    ("1-ring-logs", "Fix 1", "Logs become fixed rings, allocated once."),
    ("2-event-buffer", "Fix 2", "Callers pass in a reusable buffer for events."),
    ("3-pooled-memory", "Fix 3", "Pooled memory for book levels, queues and the order index."),
    ("4-direct-cancel", "Fix 4", "Cancel and modify go straight to the order."),
]
KEYS = ("p50", "p90", "p99", "p99_9", "p99_99", "max")


def run(path):
    d = json.load(open(path))
    out = {
        "all": {k: d["latency_ns"]["all"][k] for k in KEYS},
        "by_op": {op: {k: d["latency_ns"][op][k] for k in ("p50", "p99", "p99_9")} for op in ("add", "cross", "cancel", "modify")},
        "allocations_per_call": d["allocations_per_call"],
        "trims": d["trims"],
        "slowest": {k: d["slowest_0_1_percent"][k] for k in ("calls", "with_trim", "with_any_allocation", "with_trades", "add", "cross", "cancel", "modify")},
    }
    if "control_ns" in d:
        out["busy_loop"] = {k: d["control_ns"]["busy_loop"][k] for k in KEYS}
        out["calls_over_5us"] = d["calls_over_5us"]
    return out


def spread(ticks):
    d = json.load(open(f"bench/results/experiment-spread-{ticks}-ticks.json"))
    return {op: {k: d["latency_ns"][op][k] for k in ("p50", "p99", "p99_9")} for op in ("cancel", "modify")}


print(json.dumps({
    "machine": "AMD Ryzen 9 9900X, one pinned core, GCC 15 -O3",
    "calls_per_run": 2_000_000,
    "stages": [
        {"key": key, "label": label, "change": change,
         "runs": [run(p) for p in sorted(glob.glob(f"bench/results/{key}-run*.json"))]}
        for key, label, change in STAGES
    ],
    "spread_test": {"ticks_10": spread(10), "ticks_100": spread(100)},
}, indent=1))
