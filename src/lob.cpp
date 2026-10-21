#include "rtrade/lob.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace rtrade {


LimitOrderBook::LimitOrderBook() = default;

void LimitOrderBook::configure_circuit_breaker(double warn_pct, double trigger_pct, Price reference) {
  warn_pct_        = warn_pct;
  trigger_pct_     = trigger_pct;
  reference_price_ = reference;
  circuit_enabled_ = (warn_pct > 0.0 || trigger_pct > 0.0) && reference.ticks != 0;
  circuit_         = CircuitState::Normal;
}

void LimitOrderBook::resume_after_halt() {
  if (circuit_ == CircuitState::Triggered) {
    circuit_ = CircuitState::Normal;
  }
}

// ─── Pre-trade risk ───────────────────────────────────────────────────

void LimitOrderBook::set_risk_policy(ParticipantId p, RiskPolicy policy) {
  if (p == 0) return;
  risk_policies_[p] = policy;
}

void LimitOrderBook::unkill_participant(ParticipantId p) {
  auto it = risk_policies_.find(p);
  if (it != risk_policies_.end()) it->second.killed = false;
}

std::int64_t LimitOrderBook::net_position(ParticipantId p) const noexcept {
  auto it = net_positions_.find(p);
  return it == net_positions_.end() ? 0 : it->second;
}

RejectReason LimitOrderBook::check_risk_(const Order& o) const noexcept {
  if (o.participant == 0) return RejectReason::Unknown; // sentinel "ok"
  auto it = risk_policies_.find(o.participant);
  if (it == risk_policies_.end()) return RejectReason::Unknown;
  const RiskPolicy& pol = it->second;

  if (pol.killed) return RejectReason::RiskParticipantKilled;

  // Fat-finger qty
  if (pol.max_order_qty.units > 0 && o.qty.units > pol.max_order_qty.units) {
    return RejectReason::RiskFatFingerQty;
  }
  // Fat-finger price (only meaningful for Limit-style orders; Market has no price)
  if (o.type != OrderType::Market
      && pol.max_price_dev_from_ref.ticks > 0
      && reference_price_.ticks > 0) {
    const std::int64_t dev = std::abs(o.price.ticks - reference_price_.ticks);
    if (dev > pol.max_price_dev_from_ref.ticks) return RejectReason::RiskFatFingerPrice;
  }
  // Position limit (post-trade worst case)
  if (pol.max_net_position.units > 0) {
    auto pos_it = net_positions_.find(o.participant);
    const std::int64_t cur = pos_it == net_positions_.end() ? 0 : pos_it->second;
    const std::int64_t delta = (o.side == Side::Bid) ? o.qty.units : -o.qty.units;
    if (std::abs(cur + delta) > pol.max_net_position.units) {
      return RejectReason::RiskPositionLimit;
    }
  }
  return RejectReason::Unknown; // ok
}

// ─── Auctions ──────────────────────────────────────────────────────────

void LimitOrderBook::start_auction() {
  auction_ = AuctionState::Collecting;
}

std::optional<Price> LimitOrderBook::find_clearing_price_() const {
  if (bids_.empty() || asks_.empty()) return std::nullopt;
  // Quick eligibility check: highest bid must >= lowest ask.
  if (bids_.begin()->first < asks_.begin()->first) return std::nullopt;

  // Collect candidate prices from both books.
  std::vector<std::int64_t> prices;
  prices.reserve(bids_.size() + asks_.size());
  for (const auto& [p, _] : bids_) prices.push_back(p.ticks);
  for (const auto& [p, _] : asks_) prices.push_back(p.ticks);
  std::sort(prices.begin(), prices.end());
  prices.erase(std::unique(prices.begin(), prices.end()), prices.end());

  Price best{0};
  std::int64_t best_matched = 0;
  for (std::int64_t pt : prices) {
    std::int64_t bid_q = 0, ask_q = 0;
    for (const auto& [bp, dq] : bids_) {
      if (bp.ticks < pt) break;
      for (const Order& o : dq) bid_q += (o.qty - o.filled).units;
    }
    for (const auto& [ap, dq] : asks_) {
      if (ap.ticks > pt) break;
      for (const Order& o : dq) ask_q += (o.qty - o.filled).units;
    }
    const std::int64_t matchable = std::min(bid_q, ask_q);
    if (matchable > best_matched) {
      best_matched = matchable;
      best = Price{pt};
    }
  }
  return best_matched > 0 ? std::optional<Price>{best} : std::nullopt;
}

std::vector<Event> LimitOrderBook::uncross_at_(Price clearing, Timestamp ts) {
  std::vector<Event> out;
  while (true) {
    // Eligible bids: price >= clearing. Eligible asks: price <= clearing.
    while (!bids_.empty() && bids_.begin()->second.empty()) bids_.erase(bids_.begin());
    while (!asks_.empty() && asks_.begin()->second.empty()) asks_.erase(asks_.begin());
    if (bids_.empty() || asks_.empty()) break;
    if (bids_.begin()->first.ticks < clearing.ticks) break;
    if (asks_.begin()->first.ticks > clearing.ticks) break;

    Order& bid = bids_.begin()->second.front();
    Order& ask = asks_.begin()->second.front();
    const Quantity rem_b = bid.qty - bid.filled;
    const Quantity rem_a = ask.qty - ask.filled;
    const Quantity qty   = (rem_b.units < rem_a.units) ? rem_b : rem_a;

    // Both orders are "makers" at the clearing price; arbitrary which is taker.
    // Use the bid as taker for the audit event so a downstream tool can tell auction trades.
    out.push_back(Trade{ask.id, bid.id, clearing, qty, ts});
    emit_md_(MdExecuteOrder{ask.id, bid.id, clearing, qty}, ts);

    apply_position_delta_(bid.participant, Side::Bid, qty.units);
    apply_position_delta_(ask.participant, Side::Ask, qty.units);

    bid.filled = bid.filled + qty;
    ask.filled = ask.filled + qty;

    if (bid.filled.units == bid.qty.units) {
      const OrderId id = bid.id;
      by_id_.erase(id);
      bids_.begin()->second.pop_front();
      --resting_count_;
      emit_md_(MdDeleteOrder{id}, ts);
    }
    if (ask.filled.units == ask.qty.units) {
      const OrderId id = ask.id;
      by_id_.erase(id);
      asks_.begin()->second.pop_front();
      --resting_count_;
      emit_md_(MdDeleteOrder{id}, ts);
    }
  }
  return out;
}

std::vector<Event> LimitOrderBook::end_auction(Timestamp ts) {
  std::vector<Event> events;
  if (auction_ == AuctionState::Collecting) {
    auction_ = AuctionState::None;
    if (auto clearing = find_clearing_price_(); clearing.has_value()) {
      events = uncross_at_(*clearing, ts);
    }
  }
  commit_(events, ts);
  return events;
}

// ─── Market data ───────────────────────────────────────────────────────

void LimitOrderBook::subscribe_market_data(MarketDataSubscriber sub) {
  md_subscribers_.push_back(std::move(sub));
}

void LimitOrderBook::emit_md_(MarketDataEvent ev, Timestamp ts) {
  MarketDataEnvelope env{md_next_seq_++, ts, std::move(ev)};
  md_log_.push_back(env);
  for (const auto& sub : md_subscribers_) sub(env);
}

void LimitOrderBook::apply_position_delta_(ParticipantId p, Side side, std::int64_t qty) noexcept {
  if (p == 0 || qty == 0) return;
  const std::int64_t delta = (side == Side::Bid) ? qty : -qty;
  net_positions_[p] += delta;
}

void LimitOrderBook::update_circuit_from_trade_(Price trade_price) noexcept {
  if (!circuit_enabled_ || reference_price_.ticks == 0) return;

  // Percent move from reference. ticks is integer; promote to double for the ratio.
  const double ref  = static_cast<double>(reference_price_.ticks);
  const double last = static_cast<double>(trade_price.ticks);
  const double pct  = std::abs((last - ref) / ref) * 100.0;

  if (pct >= trigger_pct_) {
    circuit_ = CircuitState::Triggered;
  } else if (pct >= warn_pct_) {
    // Don't downgrade out of Triggered without explicit resume.
    if (circuit_ != CircuitState::Triggered) circuit_ = CircuitState::Warning;
  } else {
    if (circuit_ != CircuitState::Triggered) circuit_ = CircuitState::Normal;
  }
}

void LimitOrderBook::commit_(const std::vector<Event>& events, Timestamp ts) {
  // Enforce non-decreasing timestamp invariant. A real engine would refuse;
  // here we clamp to last_ts_ so a client clock skew doesn't break replay,
  // and we still produce a monotone sequence.
  if (ts < last_ts_) ts = last_ts_;
  last_ts_ = ts;

  for (const Event& e : events) {
    log_.push_back(EventEnvelope{next_seq_++, ts, e});
    update_telemetry_from_event_(e);
    if (auto* t = std::get_if<Trade>(&e)) {
      update_circuit_from_trade_(t->price);
    }
  }
}

void LimitOrderBook::update_telemetry_from_event_(const Event& e) noexcept {
  if (std::holds_alternative<Ack>(e))            ++telemetry_.submits_accepted;
  else if (std::holds_alternative<CancelAck>(e)) ++telemetry_.cancels_ack;
  else if (std::holds_alternative<Modified>(e))  ++telemetry_.modifies_applied;
  else if (const Trade* t = std::get_if<Trade>(&e)) {
    ++telemetry_.trades;
    telemetry_.trade_volume += static_cast<std::uint64_t>(t->price.ticks) * static_cast<std::uint64_t>(t->qty.units);
  }
  // Rejects: we can't tell here whether it's a submit-reject or cancel-reject
  // without context. For simplicity, count *all* rejects as submit-rejects
  // (the more common case; cancel-rejects are typically rare).
  else if (std::holds_alternative<Reject>(e))    ++telemetry_.submits_rejected;
}

namespace {

// "Does maker at this price cross an incoming order at this taker limit?"
// For Market orders the taker has no price constraint.
bool crosses_for_buy(Price maker_ask, Price taker_limit, bool is_market) noexcept {
  return is_market || maker_ask.ticks <= taker_limit.ticks;
}
bool crosses_for_sell(Price maker_bid, Price taker_limit, bool is_market) noexcept {
  return is_market || maker_bid.ticks >= taker_limit.ticks;
}

template <class Book>
Quantity available_crossing_qty_buy(const Book& asks, Price taker_limit, bool is_market) noexcept {
  Quantity total{0};
  for (const auto& [price, dq] : asks) {
    if (!crosses_for_buy(price, taker_limit, is_market)) break;
    for (const Order& m : dq) total = total + (m.qty - m.filled);
  }
  return total;
}

template <class Book>
Quantity available_crossing_qty_sell(const Book& bids, Price taker_limit, bool is_market) noexcept {
  Quantity total{0};
  for (const auto& [price, dq] : bids) {
    if (!crosses_for_sell(price, taker_limit, is_market)) break;
    for (const Order& m : dq) total = total + (m.qty - m.filled);
  }
  return total;
}

} // namespace

// Walk one side, matching against an incoming taker. Mutates book + by_id_ + counts.
// Emits one Trade per maker consumed (FIFO per level, best level first).
// On self-match (same non-zero participant), applies the taker's SmpMode.
template <class OppBook, class CrossPred>
void LimitOrderBook::walk_match(OppBook& opp, Order& taker, CrossPred crosses, std::vector<Event>& out, bool& taker_cancelled) {
  taker_cancelled = false;

  while (!opp.empty() && taker.filled.units < taker.qty.units) {
    auto level_it = opp.begin();
    const Price maker_price = level_it->first;
    if (!crosses(maker_price)) break;

    auto& dq = level_it->second;
    while (!dq.empty() && taker.filled.units < taker.qty.units) {
      Order& maker = dq.front();

      // Self-match detection.
      const bool self_match =
        taker.participant != 0 && maker.participant == taker.participant;

      if (self_match && taker.smp != SmpMode::None) {
        if (taker.smp == SmpMode::CancelOldest || taker.smp == SmpMode::CancelBoth) {
          const OrderId cancelled_maker = maker.id;
          out.push_back(CancelAck{cancelled_maker, taker.ts});
          by_id_.erase(cancelled_maker);
          dq.pop_front();
          --resting_count_;
          emit_md_(MdDeleteOrder{cancelled_maker}, taker.ts);
        }
        if (taker.smp == SmpMode::CancelNewest || taker.smp == SmpMode::CancelBoth) {
          // Cancel the taker. Note: taker was never added to the L3 feed yet,
          // so no MdDeleteOrder for the taker (it never appeared on the book).
          out.push_back(CancelAck{taker.id, taker.ts});
          taker_cancelled = true;
          if (dq.empty()) opp.erase(level_it);
          return;
        }
        continue;
      }

      const Quantity rem_taker = taker.qty - taker.filled;
      const Quantity rem_maker = maker.qty - maker.filled;
      const Quantity qty = (rem_taker.units < rem_maker.units) ? rem_taker : rem_maker;

      out.push_back(Trade{maker.id, taker.id, maker_price, qty, taker.ts});
      emit_md_(MdExecuteOrder{maker.id, taker.id, maker_price, qty}, taker.ts);

      apply_position_delta_(maker.participant, maker.side, qty.units);
      apply_position_delta_(taker.participant, taker.side, qty.units);

      maker.filled = maker.filled + qty;
      taker.filled = taker.filled + qty;

      if (maker.filled.units == maker.qty.units) {
        const OrderId fully_filled = maker.id;
        by_id_.erase(maker.id);
        dq.pop_front();
        --resting_count_;
        emit_md_(MdDeleteOrder{fully_filled}, taker.ts);
      }
    }
    if (dq.empty()) opp.erase(level_it);
  }
}

// Rest the (possibly partially-filled) taker on its own side at its limit price.
void LimitOrderBook::rest(Order o) {
  const Quantity remaining = o.qty - o.filled;
  Queue& queue = (o.side == Side::Bid) ? bids_[o.price] : asks_[o.price];
  queue.push_back(o);
  by_id_.emplace(o.id, Locator{o.side, o.price, std::prev(queue.end())});
  ++resting_count_;
  emit_md_(MdAddOrder{o.id, o.side, o.price, remaining}, o.ts);
}

void LimitOrderBook::submit(Order o, std::vector<Event>& out) {
  out.clear();
  const Timestamp ts = o.ts;
  submit_impl_(std::move(o), out);
  commit_(out, ts);
}

void LimitOrderBook::cancel(OrderId id, Timestamp ts, std::vector<Event>& out) {
  out.clear();
  cancel_impl_(id, ts, out);
  commit_(out, ts);
}

void LimitOrderBook::modify(OrderId id, Price new_price, Quantity new_qty, Timestamp ts, std::vector<Event>& out) {
  out.clear();
  modify_impl_(id, new_price, new_qty, ts, out);
  commit_(out, ts);
}

std::vector<Event> LimitOrderBook::submit(Order o) {
  std::vector<Event> out;
  submit(std::move(o), out);
  return out;
}

std::vector<Event> LimitOrderBook::cancel(OrderId id, Timestamp ts) {
  std::vector<Event> out;
  cancel(id, ts, out);
  return out;
}

std::vector<Event> LimitOrderBook::modify(OrderId id, Price new_price, Quantity new_qty, Timestamp ts) {
  std::vector<Event> out;
  modify(id, new_price, new_qty, ts, out);
  return out;
}

void LimitOrderBook::modify_impl_(OrderId id, Price new_price, Quantity new_qty, Timestamp ts, std::vector<Event>& out) {
  // Modify during halt would partially-execute (cancel succeeds, replace rejects).
  // Reject up front; participants who want to pull resting orders use cancel().
  if (circuit_ == CircuitState::Triggered) {
    out.push_back(Reject{id, RejectReason::CircuitBreakerTriggered, ts});
    return;
  }

  const auto found = by_id_.find(id);
  if (found == by_id_.end()) {
    out.push_back(Reject{id, RejectReason::Unknown, ts});
    return;
  }
  Order* existing = &*found->second.at;

  // Snapshot fields before we mutate the book.
  const Side      side       = existing->side;
  const OrderType type       = existing->type;
  const Price     old_price  = existing->price;
  const Quantity  old_qty    = existing->qty;
  const Quantity  cum_filled = existing->filled;

  // Replacement qty must exceed cumulative fill; otherwise reject.
  if (new_qty.units <= cum_filled.units) {
    out.push_back(Reject{id, RejectReason::ModifyBelowFilledQty, ts});
    return;
  }

  // Fast path: same price + qty reduction (still above filled) → keep time priority.
  if (new_price == old_price && new_qty.units < old_qty.units) {
    existing->qty = new_qty;
    out.push_back(Modified{id, new_price, new_qty, ts});
    return;
  }

  // Slow path: cancel-replace. Loses time priority.
  cancel_impl_(id, ts, out);
  submit_impl_(Order{id, side, type, new_price, new_qty, /*filled=*/Quantity{0}, ts}, out);

}

void LimitOrderBook::submit_impl_(Order o, std::vector<Event>& out) {
  if (circuit_ == CircuitState::Triggered) {
    out.push_back(Reject{o.id, RejectReason::CircuitBreakerTriggered, o.ts});
    return;
  }

  if (by_id_.find(o.id) != by_id_.end()) {
    out.push_back(Reject{o.id, RejectReason::DuplicateOrderId, o.ts});
    return;
  }

  if (const RejectReason r = check_risk_(o); r != RejectReason::Unknown) {
    out.push_back(Reject{o.id, r, o.ts});
    return;
  }

  // Auction collecting phase: rest Limits + PostOnly only; reject non-rest types.
  if (auction_ == AuctionState::Collecting) {
    if (o.type == OrderType::Market) {
      out.push_back(Reject{o.id, RejectReason::Unknown, o.ts});
      return;
    }
    if (o.type == OrderType::IOC) {
      out.push_back(Reject{o.id, RejectReason::ImmediateOrCancelUnfilled, o.ts});
      return;
    }
    if (o.type == OrderType::FOK) {
      out.push_back(Reject{o.id, RejectReason::FillOrKillUnfilled, o.ts});
      return;
    }
    out.push_back(Ack{o.id, o.ts});
    rest(o);
    return;
  }

  const bool is_market = (o.type == OrderType::Market);
  const bool is_buy    = (o.side == Side::Bid);
  bool       taker_cancelled = false;

  // PostOnly: reject if it would cross at all.
  if (o.type == OrderType::PostOnly) {
    const bool would_cross = is_buy
      ? (!asks_.empty() && crosses_for_buy(asks_.begin()->first, o.price, false))
      : (!bids_.empty() && crosses_for_sell(bids_.begin()->first, o.price, false));
    if (would_cross) {
      out.push_back(Reject{o.id, RejectReason::PostOnlyWouldCross, o.ts});
      return;
    }
    out.push_back(Ack{o.id, o.ts});
    rest(o);
    return;
  }

  // FOK: must fill the whole quantity at acceptable prices, or reject.
  if (o.type == OrderType::FOK) {
    const Quantity avail = is_buy
      ? available_crossing_qty_buy(asks_, o.price, false)
      : available_crossing_qty_sell(bids_, o.price, false);
    if (avail.units < o.qty.units) {
      out.push_back(Reject{o.id, RejectReason::FillOrKillUnfilled, o.ts});
      return;
    }
    out.push_back(Ack{o.id, o.ts});
    if (is_buy) {
      walk_match(asks_, o, [&](Price m){ return crosses_for_buy(m, o.price, false); }, out, taker_cancelled);
    } else {
      walk_match(bids_, o, [&](Price m){ return crosses_for_sell(m, o.price, false); }, out, taker_cancelled);
    }
    return;
  }

  // IOC: if nothing to cross, reject. Otherwise walk and silently cancel residual.
  if (o.type == OrderType::IOC) {
    const bool can_cross = is_buy
      ? (!asks_.empty() && crosses_for_buy(asks_.begin()->first, o.price, false))
      : (!bids_.empty() && crosses_for_sell(bids_.begin()->first, o.price, false));
    if (!can_cross) {
      out.push_back(Reject{o.id, RejectReason::ImmediateOrCancelUnfilled, o.ts});
      return;
    }
    out.push_back(Ack{o.id, o.ts});
    if (is_buy) {
      walk_match(asks_, o, [&](Price m){ return crosses_for_buy(m, o.price, false); }, out, taker_cancelled);
    } else {
      walk_match(bids_, o, [&](Price m){ return crosses_for_sell(m, o.price, false); }, out, taker_cancelled);
    }
    // Residual is dropped; don't rest.
    return;
  }

  // Market: cross unconditionally; residual is dropped.
  if (is_market) {
    out.push_back(Ack{o.id, o.ts});
    if (is_buy) {
      walk_match(asks_, o, [&](Price m){ return crosses_for_buy(m, o.price, true); }, out, taker_cancelled);
    } else {
      walk_match(bids_, o, [&](Price m){ return crosses_for_sell(m, o.price, true); }, out, taker_cancelled);
    }
    return;
  }

  // Limit: cross what we can, rest the residual at the limit price.
  out.push_back(Ack{o.id, o.ts});
  if (is_buy) {
    walk_match(asks_, o, [&](Price m){ return crosses_for_buy(m, o.price, false); }, out, taker_cancelled);
  } else {
    walk_match(bids_, o, [&](Price m){ return crosses_for_sell(m, o.price, false); }, out, taker_cancelled);
  }
  if (!taker_cancelled && o.filled.units < o.qty.units) {
    rest(o);
  }
}

void LimitOrderBook::cancel_impl_(OrderId id, Timestamp ts, std::vector<Event>& out) {
  auto it = by_id_.find(id);
  if (it == by_id_.end()) {
    out.push_back(Reject{id, RejectReason::Unknown, ts});
    return;
  }

  const auto loc = it->second;
  by_id_.erase(it);

  auto erase_from = [&](auto& book) {
    auto level_it = book.find(loc.price);
    level_it->second.erase(loc.at);
    if (level_it->second.empty()) book.erase(level_it);
  };
  if (loc.side == Side::Bid) {
    erase_from(bids_);
  } else {
    erase_from(asks_);
  }

  --resting_count_;
  out.push_back(CancelAck{id, ts});
  emit_md_(MdDeleteOrder{id}, ts);
}

std::optional<Price> LimitOrderBook::best_bid() const noexcept {
  if (bids_.empty()) return std::nullopt;
  return bids_.begin()->first;
}

std::optional<Price> LimitOrderBook::best_ask() const noexcept {
  if (asks_.empty()) return std::nullopt;
  return asks_.begin()->first;
}

std::size_t LimitOrderBook::size() const noexcept {
  return resting_count_;
}

namespace {
template <class Book>
std::vector<Level> top_n(const Book& book, std::size_t n) {
  std::vector<Level> out;
  out.reserve(std::min(n, book.size()));
  for (const auto& [price, dq] : book) {
    if (out.size() >= n) break;
    Quantity total{0};
    for (const Order& o : dq) {
      total = total + (o.qty - o.filled);
    }
    out.push_back(Level{price, total, dq.size()});
  }
  return out;
}
} // namespace

std::vector<Level> LimitOrderBook::bid_levels(std::size_t n) const {
  return top_n(bids_, n);
}

std::vector<Level> LimitOrderBook::ask_levels(std::size_t n) const {
  return top_n(asks_, n);
}

} // namespace rtrade
