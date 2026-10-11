#pragma once

#include "rtrade/lob.hpp"
#include "rtrade/types.hpp"

#include <cstdint>
#include <random>
#include <vector>

namespace rtrade {

// Synthetic order-flow generator. Drives a LimitOrderBook with realistic-ish
// flow: a market-maker quoting both sides near a mean-reverting reference, plus
// liquidity-taking participants and noise. Seeded RNG → bit-for-bit replayable
// from the seed alone.
class Simulator {
 public:
  enum class Scenario : std::uint8_t {
    Normal,       // OU mean reversion around a stable reference
    NewsShock,    // single large jump in the reference mid-stream
    FlashCrash,   // continuous drift toward catastrophe
  };

  struct Config {
    std::uint64_t seed{0xC0FFEE};
    Price         reference{10'000};   // tick price the OU reverts to
    double        sigma{2.0};          // OU noise (ticks per step)
    double        theta{0.05};         // mean-reversion strength
    Scenario      scenario{Scenario::Normal};
    Timestamp     dt_ns{250'000'000};  // step size, 250ms default
  };

  // Pre-registered participant ids the simulator drives.
  static constexpr ParticipantId MARKET_MAKER  = 101;
  static constexpr ParticipantId AGGRESSOR     = 102;
  static constexpr ParticipantId INFORMED      = 103;
  static constexpr ParticipantId NOISE         = 104;

  explicit Simulator(Config cfg);

  // Step the simulator forward by one tick: update the OU reference price,
  // generate a small number of orders (zero or more), submit them to lob.
  // Returns the simulator's current reference price after the step.
  Price step(LimitOrderBook& lob);

  // Run n steps. Convenience.
  void run(LimitOrderBook& lob, std::size_t n_steps);

  [[nodiscard]] Price       reference() const noexcept { return reference_; }
  [[nodiscard]] std::size_t step_count() const noexcept { return step_count_; }
  [[nodiscard]] Timestamp   now_ns() const noexcept { return now_ns_; }

 private:
  Config cfg_;
  std::mt19937_64 rng_;
  Price            reference_;
  std::size_t      step_count_{0};
  Timestamp        now_ns_{0};
  OrderId          next_order_id_{1};

  void scenario_kick_();              // scenario-specific drift / shock
  void market_maker_step_(LimitOrderBook& lob);
  void aggressor_step_(LimitOrderBook& lob);
  void informed_step_(LimitOrderBook& lob);
  void noise_step_(LimitOrderBook& lob);

  double uniform_();                  // [0, 1)
  double normal_();                   // N(0, 1)
  OrderId next_id_();
};

} // namespace rtrade
