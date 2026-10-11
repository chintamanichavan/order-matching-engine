#include "rtrade/simulator.hpp"
#include "rtrade/lob.hpp"

#include <gtest/gtest.h>

using namespace rtrade;

TEST(Simulator, NormalSenarioProducesTradesAndStaysWithinTolerance) {
  LimitOrderBook lob;
  Simulator sim(Simulator::Config{
      /*seed=*/0xC0FFEE,
      /*reference=*/Price{10'000},
      /*sigma=*/2.0,
      /*theta=*/0.05,
      /*scenario=*/Simulator::Scenario::Normal,
      /*dt_ns=*/250'000'000,
  });

  sim.run(lob, 500);

  EXPECT_GT(lob.telemetry().trades, 0u);
  // OU mean-reversion: after 500 steps, reference should stay within 2% of the mean.
  const std::int64_t drift = std::abs(sim.reference().ticks - 10'000);
  EXPECT_LT(drift, 200);
}

TEST(Simulator, DeterministicReplayFromSeed) {
  LimitOrderBook lob_a, lob_b;
  Simulator sim_a(Simulator::Config{/*seed=*/0xABCD1234});
  Simulator sim_b(Simulator::Config{/*seed=*/0xABCD1234});

  sim_a.run(lob_a, 200);
  sim_b.run(lob_b, 200);

  // Same seed → bit-for-bit same audit log.
  ASSERT_EQ(lob_a.audit_log().size(), lob_b.audit_log().size());
  for (std::size_t i = 0; i < lob_a.audit_log().size(); ++i) {
    EXPECT_EQ(lob_a.audit_log()[i].seq, lob_b.audit_log()[i].seq);
    EXPECT_EQ(lob_a.audit_log()[i].event.index(), lob_b.audit_log()[i].event.index());
  }
}

TEST(Simulator, FlashCrashDrivesPriceDown) {
  LimitOrderBook lob;
  Simulator sim(Simulator::Config{
      /*seed=*/1,
      /*reference=*/Price{10'000},
      /*sigma=*/1.0,
      /*theta=*/0.05,
      /*scenario=*/Simulator::Scenario::FlashCrash,
  });
  sim.run(lob, 400);
  // Drift reaches equilibrium against OU pull at p = ref - (drift/θ) ≈ ref − 40.
  EXPECT_LT(sim.reference().ticks, 9'990);
}
