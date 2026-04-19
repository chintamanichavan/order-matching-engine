# resilient-trade

A Rust workspace exploring the design of a fault-tolerant trading service. Split into three crates:

- **engine** — order matching and execution logic
- **api** — HTTP entrypoint (Axum)
- **risk** — pre-trade risk checks

## Stack

Tokio runtime, Axum for HTTP, Crossbeam channels, DashMap for concurrent state, Serde for serialization, Tracing for observability.

## Build

```
cargo build --workspace
cargo test --workspace
```

## Status

Exploratory — workspace scaffolding and per-crate skeletons. Not production.
