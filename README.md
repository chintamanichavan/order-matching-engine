# Order Matching Engine

Single-instrument matching engine in C++23. Price-time priority Limit Order Book, NYSE Rule 80B–style circuit breaker, deterministic replay, designed for sub-millisecond P99 on a modern CPU.

## Status

In active development. Single-thread core under construction. Public benchmarks will land once the single-thread engine and harness are honest. Until then, this README is the truth: no numbers, no promises.

## Why

- Matching engines are the small, hot, correctness-critical core of an exchange. Worth writing once from first principles.
- Sub-millisecond P99 is unglamorous and instructive. Most performance bugs in the wild are not algorithmic — they are allocation patterns, branch prediction, cache lines, and bad APIs.
- C++23 because the goal is to learn the part of systems work that's hardest to fake: ownership, allocation, memory layout, the standard library's actual cost model.

## What's in scope (v1)

- Single instrument, integer tick prices (no floats in price math).
- Order types: Limit, Market, IOC, FOK, Post-Only. Lifecycle: New / Ack / Fill / PartialFill / Cancel / Reject / Expire.
- `std::map<Price, std::deque<Order>>` for the resting book; `std::unordered_map<OrderId, Locator>` for O(log n) cancel.
- Append-only event log; deterministic replay from `(seed, event_stream)`.
- Circuit breaker (warn / trigger thresholds; halt + resume protocol).
- Google Benchmark harness + HdrHistogram_c latency capture. Methodology in `docs/BENCH.md`.

## What's deliberately out of scope (v1)

Multi-instrument matching, cross-asset, FIX/ITCH connectivity, persistence beyond an in-memory log, market-data multicast, market-maker incentive logic, fees, settlement, network layer, lock-free MT.

These are all real production concerns. They will distract from the question I want to answer first: **what is the achievable single-thread P99 on a modern CPU for a clean LOB implementation?**

Lock-free MT is the explicit v2 once the single-thread baseline is honest.

## Build

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Benchmarks (off by default):

```sh
cmake -S . -B build -G Ninja -DRTRADE_BUILD_BENCH=ON
cmake --build build
./build/bench/rtrade_bench
```

## License

No licence has been chosen yet, so all rights are reserved. The code is public so it can be read.
