#include "rtrade/lob.hpp"
#include "rtrade/simulator.hpp"

#include <benchmark/benchmark.h>

#include <random>

using namespace rtrade;

// ─── Microbench: single-order submit on a warm book ───────────────────

static void BM_SubmitLimit_FreshLevel(benchmark::State& state) {
  for (auto _ : state) {
    LimitOrderBook lob;
    Order o{1, Side::Bid, OrderType::Limit, Price{100}, Quantity{5}, Quantity{0}, 0, 0, SmpMode::None};
    benchmark::DoNotOptimize(lob.submit(o));
  }
}
BENCHMARK(BM_SubmitLimit_FreshLevel);

static void BM_SubmitLimit_WarmBook(benchmark::State& state) {
  // Pre-warm a book with 20 levels each side, 5 orders per level = 200 resting orders.
  LimitOrderBook warm;
  OrderId next_id = 1;
  for (int p = 80; p < 100; ++p) {
    for (int k = 0; k < 5; ++k) {
      Order o{next_id++, Side::Bid, OrderType::Limit, Price{p}, Quantity{1}, Quantity{0}, 0, 0, SmpMode::None};
      (void)warm.submit(o);
    }
  }
  for (int p = 101; p < 121; ++p) {
    for (int k = 0; k < 5; ++k) {
      Order o{next_id++, Side::Ask, OrderType::Limit, Price{p}, Quantity{1}, Quantity{0}, 0, 0, SmpMode::None};
      (void)warm.submit(o);
    }
  }

  // Bench: submit a limit that does NOT cross (resting), one per iteration.
  for (auto _ : state) {
    LimitOrderBook lob = warm;
    Order o{next_id, Side::Bid, OrderType::Limit, Price{90}, Quantity{1}, Quantity{0}, 0, 0, SmpMode::None};
    benchmark::DoNotOptimize(lob.submit(o));
  }
}
BENCHMARK(BM_SubmitLimit_WarmBook);

static void BM_SubmitCrossingMarket_WarmBook(benchmark::State& state) {
  LimitOrderBook warm;
  OrderId next_id = 1;
  for (int p = 101; p < 121; ++p) {
    for (int k = 0; k < 5; ++k) {
      Order o{next_id++, Side::Ask, OrderType::Limit, Price{p}, Quantity{1}, Quantity{0}, 0, 0, SmpMode::None};
      (void)warm.submit(o);
    }
  }

  for (auto _ : state) {
    LimitOrderBook lob = warm;
    // Market buy 50 units — walks 10 levels.
    Order o{next_id, Side::Bid, OrderType::Market, Price{0}, Quantity{50}, Quantity{0}, 0, 0, SmpMode::None};
    benchmark::DoNotOptimize(lob.submit(o));
  }
}
BENCHMARK(BM_SubmitCrossingMarket_WarmBook);

static void BM_Cancel_WarmBook(benchmark::State& state) {
  LimitOrderBook warm;
  for (OrderId id = 1; id <= 1000; ++id) {
    Order o{id, Side::Bid, OrderType::Limit, Price{100}, Quantity{1}, Quantity{0}, 0, 0, SmpMode::None};
    (void)warm.submit(o);
  }
  OrderId next_cancel = 1;
  for (auto _ : state) {
    state.PauseTiming();
    LimitOrderBook lob = warm;
    next_cancel = 1;
    state.ResumeTiming();
    benchmark::DoNotOptimize(lob.cancel(next_cancel++, 0));
  }
}
BENCHMARK(BM_Cancel_WarmBook);

// ─── Macrobench: simulator run, three scenarios ───────────────────────

static void BM_Sim_Normal_1000Steps(benchmark::State& state) {
  for (auto _ : state) {
    LimitOrderBook lob;
    Simulator sim{Simulator::Config{}};
    sim.run(lob, 1000);
    benchmark::DoNotOptimize(lob.size());
  }
  state.counters["trades_per_run"] = static_cast<double>(state.iterations());
}
BENCHMARK(BM_Sim_Normal_1000Steps)->Unit(benchmark::kMillisecond);

static void BM_Sim_FlashCrash_1000Steps(benchmark::State& state) {
  for (auto _ : state) {
    LimitOrderBook lob;
    Simulator sim{Simulator::Config{
        /*seed=*/0xC0FFEE,
        /*reference=*/Price{10'000},
        /*sigma=*/2.0,
        /*theta=*/0.05,
        /*scenario=*/Simulator::Scenario::FlashCrash,
    }};
    sim.run(lob, 1000);
    benchmark::DoNotOptimize(lob.size());
  }
}
BENCHMARK(BM_Sim_FlashCrash_1000Steps)->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();
