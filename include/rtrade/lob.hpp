#pragma once

#include "rtrade/bounded_log.hpp"
#include "rtrade/types.hpp"

#include <cstddef>
#include <list>
#include <functional>
#include <map>
#include <memory_resource>
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

// Single-instrument, single-threaded matching engine.
// Price-time priority. Integer-tick prices. After warm-up, submit, cancel and
// modify make no heap allocations when the caller passes in its own event
// buffer: the logs are fixed rings and the book's memory comes from a pool.
class LimitOrderBook {
public:
  LimitOrderBook();

  // Not copyable or movable: the order index holds positions inside the
  // book's own queues, which a copy would still point back into.
  LimitOrderBook(const LimitOrderBook&) = delete;
  LimitOrderBook& operator=(const LimitOrderBook&) = delete;

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

  // The same three calls, writing their events into `out` (cleared first)
  // instead of a new vector. A caller that reuses one `out` across calls
  // stops paying for an allocation on every call.
  void submit(Order o, std::vector<Event>& out);
  void cancel(OrderId id, Timestamp ts, std::vector<Event>& out);
  void modify(OrderId id, Price new_price, Quantity new_qty, Timestamp ts, std::vector<Event>& out);

  // Read-only views.
  [[nodiscard]] std::optional<Price>     best_bid() const noexcept;
  [[nodiscard]] std::optional<Price>     best_ask() const noexcept;
  [[nodiscard]] std::size_t              size() const noexcept;       // total resting orders

  // Top-N depth, deepest first (highest bids / lowest asks at index 0).
  [[nodiscard]] std::vector<Level> bid_levels(std::size_t n) const;
  [[nodiscard]] std::vector<Level> ask_levels(std::size_t n) const;

  // The most recent events the engine has emitted, oldest first (up to
  // kLogCapacity). Sequence numbers are dense and start at 1; ts_ns is
  // non-decreasing. Older entries are dropped, never shifted.
  static constexpr std::size_t kLogCapacity = 10'000;
  [[nodiscard]] const BoundedLog<EventEnvelope>& audit_log() const noexcept { return log_; }

  // ─── Market-data feed (L3 order-by-order, ITCH-style) ────────────
  using MarketDataSubscriber = std::function<void(const MarketDataEnvelope&)>;
  void subscribe_market_data(MarketDataSubscriber sub);

  // Read the cumulative L3 feed (useful for replay / debugging).
  [[nodiscard]] const BoundedLog<MarketDataEnvelope>& market_data_log() const noexcept { return md_log_; }

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
  // Append the events of a submission/cancellation/modify to `out` without
  // touching the audit log. Public wrappers commit to the log atomically.
  void submit_impl_(Order o, std::vector<Event>& out);
  void cancel_impl_(OrderId id, Timestamp ts, std::vector<Event>& out);
  void modify_impl_(OrderId id, Price new_price, Quantity new_qty, Timestamp ts, std::vector<Event>& out);

  // Append every emitted event to the audit log.
  void commit_(const std::vector<Event>& events, Timestamp ts);

  // Bids descend by price; asks ascend.
  // Each price level is a FIFO queue. A list, so that an order's position stays
  // valid while others come and go: cancel and modify go straight to it.
  using Queue = std::pmr::list<Order>;
  using BidBook = std::pmr::map<Price, Queue, std::greater<Price>>;
  using AskBook = std::pmr::map<Price, Queue>;

  // Where a resting order lives: its side and price pick the level, `at` is
  // its place in that level's queue.
  struct Locator {
    Side side;
    Price price;
    Queue::iterator at;
  };

  // Walk the opposite side matching the incoming taker until fully filled or
  // no more crossing levels remain. Emits one Trade per maker consumed.
  // taker_cancelled set true if SMP cancelled the taker mid-walk → residual
  // must not rest.
  template <class OppBook, class CrossPred>
  void walk_match(OppBook& opp, Order& taker, CrossPred crosses, std::vector<Event>& out, bool& taker_cancelled);

  // Rest a (possibly partially-filled) order on its own side at its limit price.
  void rest(Order o);

  // Price levels, their queues and the order index all take memory from one
  // pool, so memory freed by a cancel is reused by the next order instead of
  // going back to the general heap.
  std::pmr::unsynchronized_pool_resource pool_;

  BidBook bids_{&pool_};
  AskBook asks_{&pool_};

  static constexpr std::size_t kExpectedOrders = 1 << 16;  // index sized up front, so it never rehashes below this
  std::pmr::unordered_map<OrderId, Locator> by_id_{kExpectedOrders, &pool_};
  std::size_t resting_count_{0};

  BoundedLog<EventEnvelope> log_{kLogCapacity};
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
  BoundedLog<MarketDataEnvelope>   md_log_{kLogCapacity};
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
