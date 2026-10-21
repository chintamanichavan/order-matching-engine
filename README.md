# Order Matching Engine

Single-instrument matching engine in C++23. Price-time priority Limit Order Book, NYSE Rule 80B–style circuit breaker, deterministic replay, designed for sub-millisecond P99 on a modern CPU.

## Status

The single-thread engine works and has been measured. On a realistic stream of 2,000,000 adds, crosses, cancels and modifies, one call takes 90 ns at the median, 220 ns at p99 and about 310 ns at p99.9, with no heap allocation. Before the latency study it was 6.7 to 7.7 µs at p99.9. [docs/LATENCY.md](docs/LATENCY.md) has the method, what was predicted before measuring, each fix and what it changed, and what is still slow.

## Why

- Matching engines are the small, hot, correctness-critical core of an exchange. Worth writing once from first principles.
- Sub-millisecond P99 is unglamorous and instructive. Most performance bugs in the wild are not algorithmic — they are allocation patterns, branch prediction, cache lines, and bad APIs.
- C++23 because the goal is to learn the part of systems work that's hardest to fake: ownership, allocation, memory layout, the standard library's actual cost model.

## What's in scope (v1)

- Single instrument, integer tick prices (no floats in price math).
- Order types: Limit, Market, IOC, FOK, Post-Only. Lifecycle: New / Ack / Fill / PartialFill / Cancel / Reject / Expire.
- `std::pmr::map<Price, std::pmr::list<Order>>` for the resting book, memory from one pool; an order index that holds each order's place in its queue, so cancel and modify go straight to it.
- Audit and market-data logs kept as fixed rings of the latest 10,000 events; deterministic replay from `(seed, event_stream)`.
- Circuit breaker (warn / trigger thresholds; halt + resume protocol).
- A per-call latency harness (`bench/latency.cpp`) and a Google Benchmark throughput suite (`bench/lob_bench.cpp`). Method and results in `docs/LATENCY.md`.

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

Per-call latency (prints JSON; see `docs/LATENCY.md`):

```sh
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target latency
taskset -c 3 ./build-release/latency 2000000 > run.json
```

Browser demo (WebAssembly, built in the Emscripten Docker image): `wasm/build.sh`.

## License

No licence has been chosen yet, so all rights are reserved. The code is public so it can be read.
