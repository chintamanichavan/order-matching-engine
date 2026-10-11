#pragma once

#include <compare>
#include <cstdint>
#include <variant>

namespace rtrade {

// Strongly-typed integer-tick price. Tick size is venue-defined and applied
// at the UI boundary, not in the engine.
struct Price {
  std::int64_t ticks{};
  auto operator<=>(const Price&) const = default;
};

// Strongly-typed integer quantity. No floats in size math either.
struct Quantity {
  std::int64_t units{};
  auto operator<=>(const Quantity&) const = default;
};

inline Quantity operator-(Quantity a, Quantity b) noexcept { return {a.units - b.units}; }
inline Quantity operator+(Quantity a, Quantity b) noexcept { return {a.units + b.units}; }

using OrderId       = std::uint64_t;
using Timestamp     = std::int64_t; // ns since epoch
using ParticipantId = std::uint32_t;

enum class Side      : std::uint8_t { Bid, Ask };
enum class OrderType : std::uint8_t { Limit, Market, IOC, FOK, PostOnly };

enum class CircuitState : std::uint8_t { Normal, Warning, Triggered };

enum class AuctionState : std::uint8_t { None, Collecting };

// Self-match prevention modes. None = trades against own resting orders normally.
// On a self-match, the named order is cancelled (and the match is skipped).
enum class SmpMode : std::uint8_t {
  None,
  CancelNewest,  // cancel the incoming taker order (or its residual)
  CancelOldest,  // cancel the resting maker order; taker walks on
  CancelBoth,    // cancel both, no trade
};

enum class Status : std::uint8_t {
  New,
  Accepted,
  PartiallyFilled,
  Filled,
  Cancelled,
  Rejected,
  Expired,
};

struct Order {
  OrderId       id{};
  Side          side{Side::Bid};
  OrderType     type{OrderType::Limit};
  Price         price{};       // Ignored for Market
  Quantity      qty{};
  Quantity      filled{};
  Timestamp     ts{};
  ParticipantId participant{0};      // 0 = anonymous
  SmpMode       smp{SmpMode::None};
};

// ─── Events emitted by the engine ────────────────────────────────────

struct Trade {
  OrderId  maker_id;
  OrderId  taker_id;
  Price    price;
  Quantity qty;
  Timestamp ts;
};

struct Ack       { OrderId id; Timestamp ts; };
struct CancelAck { OrderId id; Timestamp ts; };
struct Modified  { OrderId id; Price new_price; Quantity new_qty; Timestamp ts; };

enum class RejectReason : std::uint8_t {
  Unknown,
  PostOnlyWouldCross,
  FillOrKillUnfilled,
  ImmediateOrCancelUnfilled,
  CircuitBreakerTriggered,
  DuplicateOrderId,
  ModifyBelowFilledQty,
  ModifyOnNonRestingOrder,
  RiskFatFingerPrice,
  RiskFatFingerQty,
  RiskPositionLimit,
  RiskParticipantKilled,
};

struct Reject { OrderId id; RejectReason reason; Timestamp ts; };

using Event = std::variant<Ack, Trade, CancelAck, Modified, Reject>;

// Append-only audit-log entry: a monotonically-sequenced, timestamped event.
// Sequence numbers are dense and start at 1. Timestamps are non-decreasing.
struct EventEnvelope {
  std::uint64_t seq;
  Timestamp     ts;     // ns, non-decreasing
  Event         event;
};

// ─── Outbound market-data feed (L3 order-by-order, ITCH-style) ────────

struct MdAddOrder     { OrderId id;        Side side; Price price; Quantity qty; };
struct MdExecuteOrder { OrderId maker_id;  OrderId taker_id; Price price; Quantity qty; };
struct MdDeleteOrder  { OrderId id; };

using MarketDataEvent = std::variant<MdAddOrder, MdExecuteOrder, MdDeleteOrder>;

struct MarketDataEnvelope {
  std::uint64_t   seq;
  Timestamp       ts;
  MarketDataEvent event;
};

// Engine-side counters for the demo / bench / observability dashboards.
struct Telemetry {
  std::uint64_t submits_accepted{0};   // Ack emitted
  std::uint64_t submits_rejected{0};   // Reject emitted from submit path
  std::uint64_t cancels_ack{0};
  std::uint64_t cancels_rejected{0};
  std::uint64_t modifies_applied{0};   // Modified emitted
  std::uint64_t trades{0};
  std::uint64_t trade_volume{0};       // sum of price.ticks * qty.units
};

} // namespace rtrade
