#include "rtrade/simulator.hpp"

#include <algorithm>
#include <cmath>

namespace rtrade {

namespace {
constexpr std::size_t MM_QUOTE_QTY     = 5;
constexpr std::int64_t MM_HALF_SPREAD  = 1;     // ticks around the reference
constexpr std::int64_t MM_REFRESH_EACH = 4;     // refresh quotes every N steps
constexpr std::int64_t INFORMED_DRIFT  = 1;     // ticks per step bias
constexpr std::size_t MAX_AGGRESSOR_QTY = 4;
constexpr std::size_t MAX_NOISE_QTY     = 2;
} // namespace

Simulator::Simulator(Config cfg)
    : cfg_(cfg)
    , rng_(cfg.seed)
    , reference_(cfg.reference) {}

double Simulator::uniform_() {
  return std::uniform_real_distribution<double>{0.0, 1.0}(rng_);
}

double Simulator::normal_() {
  return std::normal_distribution<double>{0.0, 1.0}(rng_);
}

OrderId Simulator::next_id_() {
  return next_order_id_++;
}

void Simulator::scenario_kick_() {
  // OU step on the reference price.
  // dp = θ * (μ − p) + σ * ε  (μ = cfg.reference, in ticks)
  const double pull   = cfg_.theta * static_cast<double>(cfg_.reference.ticks - reference_.ticks);
  const double noise  = cfg_.sigma * normal_();
  double next         = static_cast<double>(reference_.ticks) + pull + noise;

  // Scenario-specific overlays.
  switch (cfg_.scenario) {
    case Scenario::Normal: break;
    case Scenario::NewsShock:
      // One-time jump at step 200.
      if (step_count_ == 200) next += 80.0;  // ~0.8% jump on a 10000 ref
      break;
    case Scenario::FlashCrash:
      // Persistent downward drift after step 50.
      if (step_count_ >= 50) next -= 2.0;
      break;
  }

  reference_ = Price{static_cast<std::int64_t>(std::round(next))};
}

void Simulator::market_maker_step_(LimitOrderBook& lob) {
  // Refresh quotes periodically: cancel any existing MM quotes, post new ones
  // bracketing the reference. (For brevity we don't track ids; we just always
  // post new ones — the LOB has by_id_ + cancel/modify for a real impl.)
  if (step_count_ % MM_REFRESH_EACH != 0) return;

  Order bid{
      next_id_(), Side::Bid, OrderType::PostOnly,
      Price{reference_.ticks - MM_HALF_SPREAD}, Quantity{static_cast<std::int64_t>(MM_QUOTE_QTY)},
      Quantity{0}, now_ns_, MARKET_MAKER, SmpMode::CancelOldest};
  Order ask{
      next_id_(), Side::Ask, OrderType::PostOnly,
      Price{reference_.ticks + MM_HALF_SPREAD}, Quantity{static_cast<std::int64_t>(MM_QUOTE_QTY)},
      Quantity{0}, now_ns_, MARKET_MAKER, SmpMode::CancelOldest};
  (void)lob.submit(bid);
  (void)lob.submit(ask);
}

void Simulator::aggressor_step_(LimitOrderBook& lob) {
  // ~30% of steps: cross the spread aggressively.
  if (uniform_() >= 0.30) return;
  const Side side = uniform_() < 0.5 ? Side::Bid : Side::Ask;
  const std::int64_t qty = 1 + static_cast<std::int64_t>(uniform_() * MAX_AGGRESSOR_QTY);

  Order o{
      next_id_(), side, OrderType::Market,
      Price{0}, Quantity{qty}, Quantity{0}, now_ns_, AGGRESSOR, SmpMode::None};
  (void)lob.submit(o);
}

void Simulator::informed_step_(LimitOrderBook& lob) {
  // Only active in NewsShock / FlashCrash; takes a directional position.
  if (cfg_.scenario == Scenario::Normal) return;
  if (uniform_() >= 0.10) return;

  const Side side = (cfg_.scenario == Scenario::FlashCrash) ? Side::Ask : Side::Bid;
  Order o{
      next_id_(), side, OrderType::Limit,
      Price{reference_.ticks + ((side == Side::Bid) ? INFORMED_DRIFT : -INFORMED_DRIFT)},
      Quantity{3}, Quantity{0}, now_ns_, INFORMED, SmpMode::None};
  (void)lob.submit(o);
}

void Simulator::noise_step_(LimitOrderBook& lob) {
  // ~50% of steps a small random Limit order somewhere near the reference.
  if (uniform_() >= 0.5) return;
  const Side side = uniform_() < 0.5 ? Side::Bid : Side::Ask;
  const std::int64_t offset = static_cast<std::int64_t>(uniform_() * 8.0) - 4;  // ±4 ticks
  const std::int64_t qty    = 1 + static_cast<std::int64_t>(uniform_() * MAX_NOISE_QTY);

  Order o{
      next_id_(), side, OrderType::Limit,
      Price{reference_.ticks + offset}, Quantity{qty},
      Quantity{0}, now_ns_, NOISE, SmpMode::None};
  (void)lob.submit(o);
}

Price Simulator::step(LimitOrderBook& lob) {
  scenario_kick_();
  market_maker_step_(lob);
  aggressor_step_(lob);
  informed_step_(lob);
  noise_step_(lob);
  ++step_count_;
  now_ns_ += cfg_.dt_ns;
  return reference_;
}

void Simulator::run(LimitOrderBook& lob, std::size_t n_steps) {
  for (std::size_t i = 0; i < n_steps; ++i) step(lob);
}

} // namespace rtrade
