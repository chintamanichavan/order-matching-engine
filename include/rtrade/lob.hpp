#pragma once

#include "rtrade/types.hpp"

#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace rtrade {

// Aggregate view at a single price level — useful for depth queries.
struct Level {
  Price    price;
  Quantity total_qty;
  std::size_t order_count;
};

// Locator: where a resting order lives in the book.
// Stored in by_id_ so cancel/modify can find it in O(log n_levels) + O(1) FIFO walk.
struct OrderLocator {
  Side  side;
  Price price;
  // Index inside the deque at this price level. Cheap to recompute by scanning the
  // deque on cancel, but we keep the price+side so we know which map to look in.
};

// Single-instrument, single-threaded matching engine.
// Price-time priority. Integer-tick prices. No allocation on the hot path
// after warm-up (deque grows once, then reuses capacity).
class LimitOrderBook {
public:
  LimitOrderBook();

  // Submit an order. Returns the events generated (Ack, zero-or-more Trades,
  // or a single Reject). All events are also written to the append-only audit log.
  // Order types: Limit (rests residual), Market (drops residual), IOC (drops
  // residual), FOK (all-or-nothing pre-check), PostOnly (rejects if would cross).
  [[nodiscard]] std::vector<Event> submit(Order o);

  // Cancel a resting order by id. Returns Reject if id is unknown,
  // CancelAck otherwise. Audit-logged.
  [[nodiscard]] std::vector<Event> cancel(OrderId id, Timestamp ts);

  // Modify (cancel-replace) a resting order.
  //   - new_qty < remaining qty AND new_price == old price → in-place reduction,
  //     keeps time priority. Emits [Modified].
  //   - else → loses time priority: cancel old + submit new (may cross immediately).
  //     Emits [CancelAck, Ack, Trade*, ...].
  //   - new_qty <= cumulative filled → Reject (ModifyBelowFilledQty).
  //   - unknown id → Reject (Unknown).
  [[nodiscard]] std::vector<Event> modify(OrderId id, Price new_price, Quantity new_qty, Timestamp ts);

  // Read-only views.
  [[nodiscard]] std::optional<Price>     best_bid() const noexcept;
  [[nodiscard]] std::optional<Price>     best_ask() const noexcept;
  [[nodiscard]] std::size_t              size() const noexcept;       // total resting orders

  // Top-N depth, deepest first (highest bids / lowest asks at index 0).
  [[nodiscard]] std::vector<Level> bid_levels(std::size_t n) const;
  [[nodiscard]] std::vector<Level> ask_levels(std::size_t n) const;

  // Append-only audit log: every event the engine has emitted, in order.
  // Sequence numbers are dense and start at 1. ts_ns is non-decreasing.
  [[nodiscard]] const std::vector<EventEnvelope>& audit_log() const noexcept { return log_; }

  // ─── Market-data feed (L3 order-by-order, ITCH-style) ────────────
  using MarketDataSubscriber = std::function<void(const MarketDataEnvelope&)>;
  void subscribe_market_data(MarketDataSubscriber sub);

  // Read the cumulative L3 feed (useful for replay / debugging).
  [[nodiscard]] const std::vector<MarketDataEnvelope>& market_data_log() const noexcept { return md_log_; }

  // ─── Telemetry ───────────────────────────────────────────────────
  [[nodiscard]] const Telemetry& telemetry() const noexcept { return telemetry_; }

  // ─── Circuit breaker ─────────────────────────────────────────────
  // Configure thresholds in percent (e.g. 0.3 and 0.6 for warn/trigger).
  // reference is the price the breaker measures moves against (typically the
  // session open or a rolling 5-min anchor; supplied by the caller).
  void configure_circuit_breaker(double warn_pct, double trigger_pct, Price reference);

  [[nodiscard]] CircuitState circuit_state() const noexcept { return circuit_; }

  // Resume from Triggered → Normal. Reference price is unchanged.
  void resume_after_halt();

  // ─── Auctions ────────────────────────────────────────────────────
  // Open/close/halt-recovery auctions all use the same uncrossing mechanism.
  // During an auction, submitted Limit orders rest (no continuous matching).
  // Non-rest order types reject (Market/IOC/FOK).
  // end_auction() finds the clearing price that maximises matched volume and
  // crosses all eligible orders FIFO at that price. Returns trade events.
  void start_auction();
  [[nodiscard]] AuctionState auction_state() const noexcept { return auction_; }
  [[nodiscard]] std::vector<Event> end_auction(Timestamp ts);

  // ─── Pre-trade risk ──────────────────────────────────────────────
  // Per-participant policy. Defaults (max ticks, max qty) are "off".
  struct RiskPolicy {
    Price    max_price_dev_from_ref;  // |price - reference| > this → fat-finger reject (0 = off)
    Quantity max_order_qty;           // qty > this → reject (0 = off)
    Quantity max_net_position;        // |net long − short| > this → reject (0 = off)
    bool     killed;                  // master kill switch
  };

  // Set or update the policy for a participant. participant=0 (anonymous) ignored.
  void set_risk_policy(ParticipantId participant, RiskPolicy policy);
  // Reset the kill switch.
  void unkill_participant(ParticipantId participant);

  // Read current realised net position for a participant (signed: + long, − short).
  [[nodiscard]] std::int64_t net_position(ParticipantId participant) const noexcept;

private:
  // Build events for a submission/cancellation/modify without touching the
  // audit log. Public wrappers commit to the log atomically.
  std::vector<Event> submit_impl_(Order o);
  std::vector<Event> cancel_impl_(OrderId id, Timestamp ts);
  std::vector<Event> modify_impl_(OrderId id, Price new_price, Quantity new_qty, Timestamp ts);

  // Find the resting Order matching id; returns nullptr if not found.
  [[nodiscard]] const Order* find_resting_(OrderId id) const;

  // Append every emitted event to the audit log, return a copy for the caller.
  std::vector<Event> commit_(std::vector<Event> events, Timestamp ts);

  // Bids descend by price; asks ascend.
  using BidBook = std::map<Price, std::deque<Order>, std::greater<Price>>;
  using AskBook = std::map<Price, std::deque<Order>>;

  // Walk the opposite side matching the incoming taker until fully filled or
  // no more crossing levels remain. Emits one Trade per maker consumed.
  // taker_cancelled set true if SMP cancelled the taker mid-walk → residual
  // must not rest.
  template <class OppBook, class CrossPred>
  void walk_match(OppBook& opp, Order& taker, CrossPred crosses, std::vector<Event>& out, bool& taker_cancelled);

  // Rest a (possibly partially-filled) order on its own side at its limit price.
  void rest(Order o);

  BidBook bids_;
  AskBook asks_;

  std::unordered_map<OrderId, OrderLocator> by_id_;
  std::size_t resting_count_{0};

  std::vector<EventEnvelope> log_;
  std::uint64_t              next_seq_{1};
  Timestamp                  last_ts_{0};

  // Circuit breaker
  CircuitState circuit_{CircuitState::Normal};
  Price        reference_price_{0};
  double       warn_pct_{0.0};
  double       trigger_pct_{0.0};
  bool         circuit_enabled_{false};

  // Update the breaker state based on a fresh trade price.
  void update_circuit_from_trade_(Price trade_price) noexcept;

  // Pre-trade risk
  std::unordered_map<ParticipantId, RiskPolicy>   risk_policies_;
  std::unordered_map<ParticipantId, std::int64_t> net_positions_;

  // Returns RejectReason::Unknown if order passes; otherwise the violation.
  [[nodiscard]] RejectReason check_risk_(const Order& o) const noexcept;
  void apply_position_delta_(ParticipantId p, Side side, std::int64_t qty) noexcept;

  // Market data feed
  std::vector<MarketDataSubscriber> md_subscribers_;
  std::vector<MarketDataEnvelope>   md_log_;
  std::uint64_t                     md_next_seq_{1};

  void emit_md_(MarketDataEvent ev, Timestamp ts);

  // Telemetry
  Telemetry telemetry_;
  void update_telemetry_from_event_(const Event& e) noexcept;

  // Auctions
  AuctionState auction_{AuctionState::None};

  // Find the clearing price that maximises matched volume across the resting book.
  // Returns nullopt if no crossing is possible.
  [[nodiscard]] std::optional<Price> find_clearing_price_() const;
  std::vector<Event> uncross_at_(Price clearing, Timestamp ts);
};

} // namespace rtrade
