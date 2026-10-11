// Emscripten/embind bindings for the order matching engine.
// Exposes LimitOrderBook + Simulator + types to JavaScript so the browser
// portfolio demo can drive the real C++ engine via WASM.

#include "rtrade/lob.hpp"
#include "rtrade/simulator.hpp"

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace emscripten;
using namespace rtrade;

namespace {

// ─── JS-friendly DTOs ─────────────────────────────────────────────────
// embind binds POD structs nicely but the std::variant Event/MarketDataEvent
// don't translate. Convert to discriminated objects.

struct JsEvent {
  std::string   kind;   // "Ack" | "Trade" | "CancelAck" | "Modified" | "Reject"
  std::uint64_t seq{0};
  double        ts_ms{0};
  // common fields (set per kind)
  std::uint64_t id{0};
  std::uint64_t maker_id{0};
  std::uint64_t taker_id{0};
  std::int64_t  price_ticks{0};
  std::int64_t  qty{0};
  std::int64_t  new_price_ticks{0};
  std::int64_t  new_qty{0};
  std::string   reject_reason;
};

const char* reason_str(RejectReason r) {
  switch (r) {
    case RejectReason::Unknown:                     return "Unknown";
    case RejectReason::PostOnlyWouldCross:          return "PostOnlyWouldCross";
    case RejectReason::FillOrKillUnfilled:          return "FillOrKillUnfilled";
    case RejectReason::ImmediateOrCancelUnfilled:   return "ImmediateOrCancelUnfilled";
    case RejectReason::CircuitBreakerTriggered:     return "CircuitBreakerTriggered";
    case RejectReason::DuplicateOrderId:            return "DuplicateOrderId";
    case RejectReason::ModifyBelowFilledQty:        return "ModifyBelowFilledQty";
    case RejectReason::ModifyOnNonRestingOrder:     return "ModifyOnNonRestingOrder";
    case RejectReason::RiskFatFingerPrice:          return "RiskFatFingerPrice";
    case RejectReason::RiskFatFingerQty:            return "RiskFatFingerQty";
    case RejectReason::RiskPositionLimit:           return "RiskPositionLimit";
    case RejectReason::RiskParticipantKilled:       return "RiskParticipantKilled";
  }
  return "Unknown";
}

JsEvent to_js_event(const EventEnvelope& env) {
  JsEvent out;
  out.seq   = env.seq;
  out.ts_ms = static_cast<double>(env.ts) / 1e6;

  std::visit([&](const auto& e) {
    using T = std::decay_t<decltype(e)>;
    if constexpr (std::is_same_v<T, Ack>) {
      out.kind = "Ack";
      out.id = e.id;
    } else if constexpr (std::is_same_v<T, Trade>) {
      out.kind = "Trade";
      out.maker_id    = e.maker_id;
      out.taker_id    = e.taker_id;
      out.price_ticks = e.price.ticks;
      out.qty         = e.qty.units;
    } else if constexpr (std::is_same_v<T, CancelAck>) {
      out.kind = "CancelAck";
      out.id = e.id;
    } else if constexpr (std::is_same_v<T, Modified>) {
      out.kind = "Modified";
      out.id              = e.id;
      out.new_price_ticks = e.new_price.ticks;
      out.new_qty         = e.new_qty.units;
    } else if constexpr (std::is_same_v<T, Reject>) {
      out.kind = "Reject";
      out.id = e.id;
      out.reject_reason = reason_str(e.reason);
    }
  }, env.event);
  return out;
}

struct JsLevel {
  std::int64_t price_ticks;
  std::int64_t total_qty;
  std::uint32_t order_count;
};

// ─── Wrapper that owns a LimitOrderBook and exposes JS-friendly methods ──

class WasmEngine {
 public:
  WasmEngine() = default;

  // Submit a Limit order. Returns JS events array.
  std::vector<JsEvent> submit_limit(std::uint64_t id, std::uint32_t side, std::int64_t price_ticks,
                                    std::int64_t qty, double ts_ms, std::uint32_t participant) {
    Order o{
        id, static_cast<Side>(side), OrderType::Limit,
        Price{price_ticks}, Quantity{qty}, Quantity{0},
        static_cast<Timestamp>(ts_ms * 1e6), participant, SmpMode::None};
    return record_(lob_.submit(o));
  }

  std::vector<JsEvent> submit_market(std::uint64_t id, std::uint32_t side, std::int64_t qty,
                                     double ts_ms, std::uint32_t participant) {
    Order o{
        id, static_cast<Side>(side), OrderType::Market,
        Price{0}, Quantity{qty}, Quantity{0},
        static_cast<Timestamp>(ts_ms * 1e6), participant, SmpMode::None};
    return record_(lob_.submit(o));
  }

  std::vector<JsEvent> cancel_order(std::uint64_t id, double ts_ms) {
    return record_(lob_.cancel(id, static_cast<Timestamp>(ts_ms * 1e6)));
  }

  std::vector<JsEvent> modify_order(std::uint64_t id, std::int64_t new_price_ticks,
                                    std::int64_t new_qty, double ts_ms) {
    return record_(lob_.modify(id, Price{new_price_ticks}, Quantity{new_qty},
                               static_cast<Timestamp>(ts_ms * 1e6)));
  }

  void configure_circuit_breaker(double warn_pct, double trigger_pct, std::int64_t ref_ticks) {
    lob_.configure_circuit_breaker(warn_pct, trigger_pct, Price{ref_ticks});
  }

  std::uint32_t circuit_state() const noexcept { return static_cast<std::uint32_t>(lob_.circuit_state()); }
  void resume_after_halt() { lob_.resume_after_halt(); }

  void start_auction() { lob_.start_auction(); }
  std::vector<JsEvent> end_auction(double ts_ms) {
    return record_(lob_.end_auction(static_cast<Timestamp>(ts_ms * 1e6)));
  }

  std::int64_t best_bid_ticks() const {
    auto b = lob_.best_bid(); return b ? b->ticks : 0;
  }
  std::int64_t best_ask_ticks() const {
    auto a = lob_.best_ask(); return a ? a->ticks : 0;
  }
  std::size_t size() const noexcept { return lob_.size(); }

  std::vector<JsLevel> bid_levels(std::size_t n) const {
    auto src = lob_.bid_levels(n);
    std::vector<JsLevel> out;
    out.reserve(src.size());
    for (const auto& l : src) out.push_back({l.price.ticks, l.total_qty.units, static_cast<std::uint32_t>(l.order_count)});
    return out;
  }
  std::vector<JsLevel> ask_levels(std::size_t n) const {
    auto src = lob_.ask_levels(n);
    std::vector<JsLevel> out;
    out.reserve(src.size());
    for (const auto& l : src) out.push_back({l.price.ticks, l.total_qty.units, static_cast<std::uint32_t>(l.order_count)});
    return out;
  }

  // Telemetry getters (avoid binding the struct directly).
  std::uint64_t telemetry_submits_accepted() const { return lob_.telemetry().submits_accepted; }
  std::uint64_t telemetry_submits_rejected() const { return lob_.telemetry().submits_rejected; }
  std::uint64_t telemetry_cancels_ack()      const { return lob_.telemetry().cancels_ack; }
  std::uint64_t telemetry_modifies_applied() const { return lob_.telemetry().modifies_applied; }
  std::uint64_t telemetry_trades()           const { return lob_.telemetry().trades; }
  std::uint64_t telemetry_trade_volume()     const { return lob_.telemetry().trade_volume; }

  std::size_t audit_log_size() const noexcept { return lob_.audit_log().size(); }
  std::size_t market_data_log_size() const noexcept { return lob_.market_data_log().size(); }

  // Simulator. Driven by stepping; the simulator owns its own RNG/clock.
  void configure_simulator(std::uint64_t seed, std::int64_t reference_ticks, double sigma,
                           double theta, std::uint32_t scenario) {
    sim_ = std::make_unique<Simulator>(Simulator::Config{
        seed, Price{reference_ticks}, sigma, theta,
        static_cast<Simulator::Scenario>(scenario)});
  }

  std::int64_t sim_step() {
    if (!sim_) return 0;
    return sim_->step(lob_).ticks;
  }

  std::int64_t sim_reference_ticks() const { return sim_ ? sim_->reference().ticks : 0; }

 private:
  LimitOrderBook            lob_;
  std::unique_ptr<Simulator> sim_;

  std::vector<JsEvent> record_(std::vector<Event> events) {
    std::vector<JsEvent> out;
    out.reserve(events.size());
    const auto& log = lob_.audit_log();
    // Map back to envelopes by taking the last events.size() entries.
    const std::size_t n = events.size();
    if (log.size() >= n) {
      for (std::size_t i = log.size() - n; i < log.size(); ++i) {
        out.push_back(to_js_event(log[i]));
      }
    }
    return out;
  }
};

} // namespace

EMSCRIPTEN_BINDINGS(rtrade_module) {
  value_object<JsEvent>("JsEvent")
      .field("kind",            &JsEvent::kind)
      .field("seq",             &JsEvent::seq)
      .field("ts_ms",           &JsEvent::ts_ms)
      .field("id",              &JsEvent::id)
      .field("maker_id",        &JsEvent::maker_id)
      .field("taker_id",        &JsEvent::taker_id)
      .field("price_ticks",     &JsEvent::price_ticks)
      .field("qty",             &JsEvent::qty)
      .field("new_price_ticks", &JsEvent::new_price_ticks)
      .field("new_qty",         &JsEvent::new_qty)
      .field("reject_reason",   &JsEvent::reject_reason);

  value_object<JsLevel>("JsLevel")
      .field("price_ticks", &JsLevel::price_ticks)
      .field("total_qty",   &JsLevel::total_qty)
      .field("order_count", &JsLevel::order_count);

  register_vector<JsEvent>("VectorJsEvent");
  register_vector<JsLevel>("VectorJsLevel");

  class_<WasmEngine>("Engine")
      .constructor<>()
      .function("submit_limit",                &WasmEngine::submit_limit)
      .function("submit_market",               &WasmEngine::submit_market)
      .function("cancel_order",                &WasmEngine::cancel_order)
      .function("modify_order",                &WasmEngine::modify_order)
      .function("configure_circuit_breaker",   &WasmEngine::configure_circuit_breaker)
      .function("circuit_state",               &WasmEngine::circuit_state)
      .function("resume_after_halt",           &WasmEngine::resume_after_halt)
      .function("start_auction",               &WasmEngine::start_auction)
      .function("end_auction",                 &WasmEngine::end_auction)
      .function("best_bid_ticks",              &WasmEngine::best_bid_ticks)
      .function("best_ask_ticks",              &WasmEngine::best_ask_ticks)
      .function("size",                        &WasmEngine::size)
      .function("bid_levels",                  &WasmEngine::bid_levels)
      .function("ask_levels",                  &WasmEngine::ask_levels)
      .function("telemetry_submits_accepted",  &WasmEngine::telemetry_submits_accepted)
      .function("telemetry_submits_rejected",  &WasmEngine::telemetry_submits_rejected)
      .function("telemetry_cancels_ack",       &WasmEngine::telemetry_cancels_ack)
      .function("telemetry_modifies_applied",  &WasmEngine::telemetry_modifies_applied)
      .function("telemetry_trades",            &WasmEngine::telemetry_trades)
      .function("telemetry_trade_volume",      &WasmEngine::telemetry_trade_volume)
      .function("audit_log_size",              &WasmEngine::audit_log_size)
      .function("market_data_log_size",        &WasmEngine::market_data_log_size)
      .function("configure_simulator",         &WasmEngine::configure_simulator)
      .function("sim_step",                    &WasmEngine::sim_step)
      .function("sim_reference_ticks",         &WasmEngine::sim_reference_ticks);
}
