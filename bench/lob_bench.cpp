// Throughput of single calls and of whole simulator runs. For the latency of
// each call and its slow tail, see latency.cpp and docs/LATENCY.md.
//
// The engine cannot be copied, so each benchmark undoes its own work: a
// timed loop that rests an order also cancels it, and the timings below are
// for the pair.

#include "rtrade/lob.hpp"
#include "rtrade/simulator.hpp"

#include <benchmark/benchmark.h>

#include <memory>
#include <vector>

using namespace rtrade;

namespace {

Order limit(OrderId id, Side side, std::int64_t price) {
  return Order{id, side, OrderType::Limit, Price{price}, Quantity{1}, Quantity{0}, 0, 0, SmpMode::None};
}

// 20 levels each side, 5 orders per level: 200 resting orders. Returns the next free id.
OrderId fill_both_sides(LimitOrderBook& lob, std::vector<Event>& out) {
  OrderId id = 1;
  for (std::int64_t p = 80; p < 100; ++p) for (int k = 0; k < 5; ++k) lob.submit(limit(id++, Side::Bid, p), out);
  for (std::int64_t p = 101; p < 121; ++p) for (int k = 0; k < 5; ++k) lob.submit(limit(id++, Side::Ask, p), out);
  return id;
}

}  // namespace

// ─── Microbench: single calls on a warm book ──────────────────────────

static void BM_RestThenCancel_NewLevel(benchmark::State& state) {
  LimitOrderBook lob;
  std::vector<Event> out;
  const OrderId id = fill_both_sides(lob, out);
  for (auto _ : state) {
    lob.submit(limit(id, Side::Bid, 50), out);  // a price with no level yet
    lob.cancel(id, 0, out);
    benchmark::DoNotOptimize(out.data());
  }
}
BENCHMARK(BM_RestThenCancel_NewLevel);

static void BM_RestThenCancel_WarmLevel(benchmark::State& state) {
  LimitOrderBook lob;
  std::vector<Event> out;
  const OrderId id = fill_both_sides(lob, out);
  for (auto _ : state) {
    lob.submit(limit(id, Side::Bid, 90), out);  // joins the back of a level of 5
    lob.cancel(id, 0, out);
    benchmark::DoNotOptimize(out.data());
  }
}
BENCHMARK(BM_RestThenCancel_WarmLevel);

static void BM_CancelThenRest_DeepLevel(benchmark::State& state) {
  // One level of 1,000 orders; cancel the oldest and rest it again at the back.
  LimitOrderBook lob;
  std::vector<Event> out;
  for (OrderId id = 1; id <= 1000; ++id) lob.submit(limit(id, Side::Bid, 100), out);
  OrderId next = 1;
  for (auto _ : state) {
    lob.cancel(next, 0, out);
    lob.submit(limit(next, Side::Bid, 100), out);
    next = next == 1000 ? 1 : next + 1;
    benchmark::DoNotOptimize(out.data());
  }
}
BENCHMARK(BM_CancelThenRest_DeepLevel);

static void BM_MarketBuy50_TenLevels(benchmark::State& state) {
  // Market buy of 50 units walks 10 ask levels of 5. The levels are put back
  // with the timer paused.
  LimitOrderBook lob;
  std::vector<Event> out;
  OrderId id = fill_both_sides(lob, out);
  for (auto _ : state) {
    lob.submit(Order{id++, Side::Bid, OrderType::Market, Price{0}, Quantity{50}, Quantity{0}, 0, 0, SmpMode::None}, out);
    benchmark::DoNotOptimize(out.data());
    state.PauseTiming();
    for (std::int64_t p = 101; p < 111; ++p) for (int k = 0; k < 5; ++k) lob.submit(limit(id++, Side::Ask, p), out);
    state.ResumeTiming();
  }
}
BENCHMARK(BM_MarketBuy50_TenLevels);

// ─── Macrobench: simulator run, two scenarios ─────────────────────────
// A new book per run; setting it up (its logs and index are allocated in
// full) is not timed.

static void BM_Sim_Normal_1000Steps(benchmark::State& state) {
  for (auto _ : state) {
    state.PauseTiming();
    auto lob = std::make_unique<LimitOrderBook>();
    state.ResumeTiming();
    Simulator sim{Simulator::Config{}};
    sim.run(*lob, 1000);
    benchmark::DoNotOptimize(lob->size());
    state.PauseTiming();
    lob.reset();
    state.ResumeTiming();
  }
}
BENCHMARK(BM_Sim_Normal_1000Steps)->Unit(benchmark::kMicrosecond);

static void BM_Sim_FlashCrash_1000Steps(benchmark::State& state) {
  for (auto _ : state) {
    state.PauseTiming();
    auto lob = std::make_unique<LimitOrderBook>();
    state.ResumeTiming();
    Simulator sim{Simulator::Config{
        /*seed=*/0xC0FFEE,
        /*reference=*/Price{10'000},
        /*sigma=*/2.0,
        /*theta=*/0.05,
        /*scenario=*/Simulator::Scenario::FlashCrash,
    }};
    sim.run(*lob, 1000);
    benchmark::DoNotOptimize(lob->size());
    state.PauseTiming();
    lob.reset();
    state.ResumeTiming();
  }
}
BENCHMARK(BM_Sim_FlashCrash_1000Steps)->Unit(benchmark::kMicrosecond);

static void BM_ConstructBook(benchmark::State& state) {
  // The price of allocating everything up front: paid once, at start.
  for (auto _ : state) {
    LimitOrderBook lob;
    benchmark::DoNotOptimize(&lob);
  }
}
BENCHMARK(BM_ConstructBook)->Unit(benchmark::kMicrosecond);

BENCHMARK_MAIN();
