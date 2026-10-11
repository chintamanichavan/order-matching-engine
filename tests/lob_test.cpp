#include "rtrade/lob.hpp"

#include <gtest/gtest.h>

#include <variant>

using namespace rtrade;

namespace {

Order make_limit(OrderId id, Side side, std::int64_t price_ticks, std::int64_t qty, Timestamp ts = 0) {
  return Order{id, side, OrderType::Limit, Price{price_ticks}, Quantity{qty}, Quantity{0}, ts};
}

Order make_market(OrderId id, Side side, std::int64_t qty, Timestamp ts = 0) {
  return Order{id, side, OrderType::Market, Price{0}, Quantity{qty}, Quantity{0}, ts};
}

Order make_ioc(OrderId id, Side side, std::int64_t price_ticks, std::int64_t qty, Timestamp ts = 0) {
  return Order{id, side, OrderType::IOC, Price{price_ticks}, Quantity{qty}, Quantity{0}, ts};
}

Order make_fok(OrderId id, Side side, std::int64_t price_ticks, std::int64_t qty, Timestamp ts = 0) {
  return Order{id, side, OrderType::FOK, Price{price_ticks}, Quantity{qty}, Quantity{0}, ts};
}

Order make_post_only(OrderId id, Side side, std::int64_t price_ticks, std::int64_t qty, Timestamp ts = 0) {
  return Order{id, side, OrderType::PostOnly, Price{price_ticks}, Quantity{qty}, Quantity{0}, ts};
}

template <class T>
bool holds(const Event& e) { return std::holds_alternative<T>(e); }

// Count Trade events and accumulate filled qty.
struct TradeSummary { std::size_t count{0}; std::int64_t total_qty{0}; };
TradeSummary summarise_trades(const std::vector<Event>& events) {
  TradeSummary s;
  for (const Event& e : events) {
    if (auto* t = std::get_if<Trade>(&e)) {
      ++s.count;
      s.total_qty += t->qty.units;
    }
  }
  return s;
}

} // namespace

// ─── Empty book / resting / FIFO / cancel — Phase 1 stubs ──────────────

TEST(LimitOrderBookSubmit, FreshBookIsEmpty) {
  LimitOrderBook lob;
  EXPECT_EQ(lob.size(), 0u);
  EXPECT_FALSE(lob.best_bid().has_value());
  EXPECT_FALSE(lob.best_ask().has_value());
}

TEST(LimitOrderBookSubmit, SingleLimitRestsAndAcks) {
  LimitOrderBook lob;
  auto events = lob.submit(make_limit(/*id=*/1, Side::Bid, /*price=*/100, /*qty=*/5));

  ASSERT_EQ(events.size(), 1u);
  EXPECT_TRUE(holds<Ack>(events[0]));
  EXPECT_EQ(std::get<Ack>(events[0]).id, 1u);
  EXPECT_EQ(lob.size(), 1u);
  ASSERT_TRUE(lob.best_bid().has_value());
  EXPECT_EQ(lob.best_bid()->ticks, 100);
  EXPECT_FALSE(lob.best_ask().has_value());
}

TEST(LimitOrderBookSubmit, BidsDescendAsksAscend) {
  LimitOrderBook lob;
  (void)lob.submit(make_limit(1, Side::Bid, 95, 1));
  (void)lob.submit(make_limit(2, Side::Bid, 100, 1));
  (void)lob.submit(make_limit(3, Side::Bid, 98, 1));
  EXPECT_EQ(lob.best_bid()->ticks, 100);

  (void)lob.submit(make_limit(10, Side::Ask, 110, 1));
  (void)lob.submit(make_limit(11, Side::Ask, 105, 1));
  (void)lob.submit(make_limit(12, Side::Ask, 107, 1));
  EXPECT_EQ(lob.best_ask()->ticks, 105);
}

TEST(LimitOrderBookSubmit, DuplicateIdRejects) {
  LimitOrderBook lob;
  (void)lob.submit(make_limit(42, Side::Bid, 100, 1));
  auto events = lob.submit(make_limit(42, Side::Bid, 100, 1));
  ASSERT_EQ(events.size(), 1u);
  ASSERT_TRUE(holds<Reject>(events[0]));
  EXPECT_EQ(std::get<Reject>(events[0]).reason, RejectReason::DuplicateOrderId);
}

TEST(LimitOrderBookSubmit, FifoAtPriceLevel) {
  LimitOrderBook lob;
  (void)lob.submit(make_limit(1, Side::Bid, 100, 3));
  (void)lob.submit(make_limit(2, Side::Bid, 100, 4));
  (void)lob.submit(make_limit(3, Side::Bid, 100, 5));

  auto levels = lob.bid_levels(1);
  ASSERT_EQ(levels.size(), 1u);
  EXPECT_EQ(levels[0].price.ticks, 100);
  EXPECT_EQ(levels[0].total_qty.units, 12);
  EXPECT_EQ(levels[0].order_count, 3u);
}

TEST(LimitOrderBookCancel, RemovesRestingOrder) {
  LimitOrderBook lob;
  (void)lob.submit(make_limit(1, Side::Bid, 100, 5));
  (void)lob.submit(make_limit(2, Side::Bid, 100, 3));

  auto events = lob.cancel(1, 0);
  ASSERT_EQ(events.size(), 1u);
  ASSERT_TRUE(holds<CancelAck>(events[0]));

  auto levels = lob.bid_levels(1);
  ASSERT_EQ(levels.size(), 1u);
  EXPECT_EQ(levels[0].order_count, 1u);
  EXPECT_EQ(levels[0].total_qty.units, 3);
}

TEST(LimitOrderBookCancel, UnknownIdRejects) {
  LimitOrderBook lob;
  auto events = lob.cancel(999, 0);
  ASSERT_EQ(events.size(), 1u);
  ASSERT_TRUE(holds<Reject>(events[0]));
}

// ─── Matching: Limit-vs-Limit crossing ─────────────────────────────────

TEST(Matching, BuyLimitCrossesRestingAskAtMakerPrice) {
  LimitOrderBook lob;
  (void)lob.submit(make_limit(1, Side::Ask, 100, 5));  // maker
  auto events = lob.submit(make_limit(2, Side::Bid, 100, 3)); // taker

  // Expect Ack(taker) + Trade(qty=3 at maker price 100). Taker fully filled.
  ASSERT_EQ(events.size(), 2u);
  EXPECT_TRUE(holds<Ack>(events[0]));
  ASSERT_TRUE(holds<Trade>(events[1]));
  const Trade& t = std::get<Trade>(events[1]);
  EXPECT_EQ(t.maker_id, 1u);
  EXPECT_EQ(t.taker_id, 2u);
  EXPECT_EQ(t.price.ticks, 100);
  EXPECT_EQ(t.qty.units, 3);
  EXPECT_EQ(lob.size(), 1u);  // 5 - 3 = 2 left on the ask
  EXPECT_EQ(lob.best_ask()->ticks, 100);
}

TEST(Matching, CrossingPriceIsMakerPrice) {
  // Taker willing to pay 105; resting ask at 100. Trade should print at 100, not 105.
  LimitOrderBook lob;
  (void)lob.submit(make_limit(1, Side::Ask, 100, 1));
  auto events = lob.submit(make_limit(2, Side::Bid, 105, 1));
  auto s = summarise_trades(events);
  EXPECT_EQ(s.count, 1u);
  EXPECT_EQ(s.total_qty, 1);
  EXPECT_EQ(std::get<Trade>(events[1]).price.ticks, 100);
}

TEST(Matching, MultiLevelWalk) {
  LimitOrderBook lob;
  (void)lob.submit(make_limit(1, Side::Ask, 100, 2));
  (void)lob.submit(make_limit(2, Side::Ask, 101, 3));
  (void)lob.submit(make_limit(3, Side::Ask, 102, 4));

  // Taker wants 6 @ <= 101. Expect to fill 2 @ 100 and 3 @ 101. Residual 1 rests at 101.
  auto events = lob.submit(make_limit(99, Side::Bid, 101, 6));
  auto s = summarise_trades(events);
  EXPECT_EQ(s.count, 2u);
  EXPECT_EQ(s.total_qty, 5);

  // Residual rests at the taker's limit (101), even though the level was just emptied.
  EXPECT_EQ(lob.size(), 2u);  // resting: 4@102 ask + 1@101 bid (the taker residual)
  ASSERT_TRUE(lob.best_bid().has_value());
  EXPECT_EQ(lob.best_bid()->ticks, 101);
  EXPECT_EQ(lob.best_ask()->ticks, 102);
}

TEST(Matching, PartialFillThenRest) {
  LimitOrderBook lob;
  (void)lob.submit(make_limit(1, Side::Ask, 100, 2));
  auto events = lob.submit(make_limit(2, Side::Bid, 100, 5));

  auto s = summarise_trades(events);
  EXPECT_EQ(s.count, 1u);
  EXPECT_EQ(s.total_qty, 2);
  // Residual 3 rests at bid 100.
  EXPECT_EQ(lob.size(), 1u);
  EXPECT_EQ(lob.best_bid()->ticks, 100);
  EXPECT_FALSE(lob.best_ask().has_value());
}

TEST(Matching, FifoAtPriceLevelOnTakerHit) {
  // Three makers at 100 in arrival order. A taker that wants only 1 hits the first maker.
  LimitOrderBook lob;
  (void)lob.submit(make_limit(1, Side::Ask, 100, 1));
  (void)lob.submit(make_limit(2, Side::Ask, 100, 2));
  (void)lob.submit(make_limit(3, Side::Ask, 100, 3));

  auto events = lob.submit(make_limit(99, Side::Bid, 100, 1));
  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(std::get<Trade>(events[1]).maker_id, 1u);  // FIFO: oldest matched first

  auto levels = lob.ask_levels(1);
  ASSERT_EQ(levels.size(), 1u);
  EXPECT_EQ(levels[0].total_qty.units, 5);  // 2 + 3 remaining at this level
  EXPECT_EQ(levels[0].order_count, 2u);
}

// ─── Market ────────────────────────────────────────────────────────────

TEST(Market, FullFillFromSingleLevel) {
  LimitOrderBook lob;
  (void)lob.submit(make_limit(1, Side::Ask, 100, 10));
  auto events = lob.submit(make_market(99, Side::Bid, 4));

  auto s = summarise_trades(events);
  EXPECT_EQ(s.count, 1u);
  EXPECT_EQ(s.total_qty, 4);
  EXPECT_EQ(lob.size(), 1u);
}

TEST(Market, WalksMultipleLevels) {
  LimitOrderBook lob;
  (void)lob.submit(make_limit(1, Side::Ask, 100, 2));
  (void)lob.submit(make_limit(2, Side::Ask, 101, 2));
  (void)lob.submit(make_limit(3, Side::Ask, 102, 2));

  auto events = lob.submit(make_market(99, Side::Bid, 5));
  auto s = summarise_trades(events);
  EXPECT_EQ(s.count, 3u);
  EXPECT_EQ(s.total_qty, 5);
  EXPECT_EQ(lob.size(), 1u);  // last maker has 1 left
}

TEST(Market, NoLiquidityNoFills) {
  LimitOrderBook lob;
  auto events = lob.submit(make_market(99, Side::Bid, 5));
  // Ack but no trades; residual is dropped (Market doesn't rest).
  ASSERT_FALSE(events.empty());
  EXPECT_TRUE(holds<Ack>(events[0]));
  auto s = summarise_trades(events);
  EXPECT_EQ(s.count, 0u);
  EXPECT_EQ(lob.size(), 0u);
}

TEST(Market, PartialResidualNotRested) {
  LimitOrderBook lob;
  (void)lob.submit(make_limit(1, Side::Ask, 100, 2));
  auto events = lob.submit(make_market(99, Side::Bid, 5));

  auto s = summarise_trades(events);
  EXPECT_EQ(s.total_qty, 2);
  EXPECT_EQ(lob.size(), 0u);  // book empty; Market residual is dropped
  EXPECT_FALSE(lob.best_bid().has_value());
}

// ─── IOC ───────────────────────────────────────────────────────────────

TEST(IOC, FullMatch) {
  LimitOrderBook lob;
  (void)lob.submit(make_limit(1, Side::Ask, 100, 5));
  auto events = lob.submit(make_ioc(99, Side::Bid, 100, 3));
  auto s = summarise_trades(events);
  EXPECT_EQ(s.total_qty, 3);
  EXPECT_EQ(lob.size(), 1u);
}

TEST(IOC, PartialMatchResidualDropped) {
  LimitOrderBook lob;
  (void)lob.submit(make_limit(1, Side::Ask, 100, 2));
  auto events = lob.submit(make_ioc(99, Side::Bid, 100, 5));
  auto s = summarise_trades(events);
  EXPECT_EQ(s.total_qty, 2);
  EXPECT_EQ(lob.size(), 0u);                  // no residual rest
  EXPECT_FALSE(lob.best_bid().has_value());
}

TEST(IOC, NoCrossRejects) {
  LimitOrderBook lob;
  (void)lob.submit(make_limit(1, Side::Ask, 105, 5));
  auto events = lob.submit(make_ioc(99, Side::Bid, 100, 5));
  ASSERT_EQ(events.size(), 1u);
  ASSERT_TRUE(holds<Reject>(events[0]));
  EXPECT_EQ(std::get<Reject>(events[0]).reason, RejectReason::ImmediateOrCancelUnfilled);
}

// ─── FOK ───────────────────────────────────────────────────────────────

TEST(FOK, FullMatchAtSingleLevel) {
  LimitOrderBook lob;
  (void)lob.submit(make_limit(1, Side::Ask, 100, 5));
  auto events = lob.submit(make_fok(99, Side::Bid, 100, 5));
  auto s = summarise_trades(events);
  EXPECT_EQ(s.total_qty, 5);
  EXPECT_EQ(lob.size(), 0u);
}

TEST(FOK, FullMatchAcrossLevels) {
  LimitOrderBook lob;
  (void)lob.submit(make_limit(1, Side::Ask, 100, 2));
  (void)lob.submit(make_limit(2, Side::Ask, 101, 3));
  auto events = lob.submit(make_fok(99, Side::Bid, 101, 5));
  auto s = summarise_trades(events);
  EXPECT_EQ(s.count, 2u);
  EXPECT_EQ(s.total_qty, 5);
  EXPECT_EQ(lob.size(), 0u);
}

TEST(FOK, InsufficientLiquidityRejectsNoTradeEmitted) {
  LimitOrderBook lob;
  (void)lob.submit(make_limit(1, Side::Ask, 100, 2));
  (void)lob.submit(make_limit(2, Side::Ask, 101, 2));
  auto events = lob.submit(make_fok(99, Side::Bid, 101, 5));
  ASSERT_EQ(events.size(), 1u);
  ASSERT_TRUE(holds<Reject>(events[0]));
  EXPECT_EQ(std::get<Reject>(events[0]).reason, RejectReason::FillOrKillUnfilled);
  // Makers untouched.
  EXPECT_EQ(lob.size(), 2u);
  EXPECT_EQ(lob.ask_levels(1)[0].total_qty.units, 2);
}

TEST(FOK, PriceLimitIgnoresFartherLevels) {
  // FOK buy at 100, but liquidity at 101 is far enough that aggregate < qty at the
  // acceptable price (100 only). Should reject.
  LimitOrderBook lob;
  (void)lob.submit(make_limit(1, Side::Ask, 100, 2));
  (void)lob.submit(make_limit(2, Side::Ask, 101, 10));
  auto events = lob.submit(make_fok(99, Side::Bid, 100, 5));
  ASSERT_EQ(events.size(), 1u);
  EXPECT_TRUE(holds<Reject>(events[0]));
  EXPECT_EQ(lob.ask_levels(2)[0].total_qty.units, 2);
}

// ─── PostOnly ──────────────────────────────────────────────────────────

TEST(PostOnly, RestsWhenNoCross) {
  LimitOrderBook lob;
  (void)lob.submit(make_limit(1, Side::Ask, 105, 1));
  auto events = lob.submit(make_post_only(99, Side::Bid, 100, 5));
  ASSERT_EQ(events.size(), 1u);
  EXPECT_TRUE(holds<Ack>(events[0]));
  EXPECT_EQ(lob.size(), 2u);
  EXPECT_EQ(lob.best_bid()->ticks, 100);
}

TEST(PostOnly, RejectsIfWouldCross) {
  LimitOrderBook lob;
  (void)lob.submit(make_limit(1, Side::Ask, 100, 5));
  auto events = lob.submit(make_post_only(99, Side::Bid, 100, 5));  // would take
  ASSERT_EQ(events.size(), 1u);
  ASSERT_TRUE(holds<Reject>(events[0]));
  EXPECT_EQ(std::get<Reject>(events[0]).reason, RejectReason::PostOnlyWouldCross);
  EXPECT_EQ(lob.size(), 1u);  // maker untouched, taker not rested
}

TEST(PostOnly, RejectsIfWouldCrossAtBetterPrice) {
  // Taker bid 105 vs maker ask 100 — bid crosses through the ask, must reject.
  LimitOrderBook lob;
  (void)lob.submit(make_limit(1, Side::Ask, 100, 5));
  auto events = lob.submit(make_post_only(99, Side::Bid, 105, 5));
  ASSERT_TRUE(holds<Reject>(events[0]));
}

// ─── Auctions ──────────────────────────────────────────────────────────

namespace {
std::size_t count_trades_in_log(const std::vector<EventEnvelope>& log) {
  std::size_t n = 0;
  for (const auto& env : log) if (std::holds_alternative<Trade>(env.event)) ++n;
  return n;
}
} // namespace

TEST(Auction, OrdersRestNoContinuousMatch) {
  LimitOrderBook lob;
  lob.start_auction();

  (void)lob.submit(make_limit(1, Side::Bid, 100, 5));
  (void)lob.submit(make_limit(2, Side::Ask, 100, 5));

  EXPECT_EQ(lob.size(), 2u);
  EXPECT_EQ(count_trades_in_log(lob.audit_log()), 0u);
}

TEST(Auction, UncrossesAtMaxVolumePrice) {
  LimitOrderBook lob;
  lob.start_auction();

  // Build a book with clear max-match price at 100.
  (void)lob.submit(make_limit(1, Side::Bid, 102, 3));  // willing to pay up to 102
  (void)lob.submit(make_limit(2, Side::Bid, 100, 5));
  (void)lob.submit(make_limit(3, Side::Ask, 98,  4));  // willing to sell as low as 98
  (void)lob.submit(make_limit(4, Side::Ask, 100, 6));

  // At p=100: bids ≥ 100 sum to 3+5 = 8; asks ≤ 100 sum to 4+6 = 10. matched = 8.
  // At p=98: bids ≥ 98 sum to 3+5 = 8; asks ≤ 98 sum to 4. matched = 4.
  // So clearing should be 100.

  auto events = lob.end_auction(/*ts=*/0);
  auto s = summarise_trades(events);
  EXPECT_GE(s.total_qty, 8);  // 8 units should cross at clearing price 100

  for (const auto& e : events) {
    if (auto* t = std::get_if<Trade>(&e)) {
      EXPECT_EQ(t->price.ticks, 100);
    }
  }
  EXPECT_EQ(lob.auction_state(), AuctionState::None);
}

TEST(Auction, NoCrossWhenNoOverlap) {
  LimitOrderBook lob;
  lob.start_auction();
  (void)lob.submit(make_limit(1, Side::Bid, 99,  5));
  (void)lob.submit(make_limit(2, Side::Ask, 101, 5));
  auto events = lob.end_auction(0);
  EXPECT_EQ(summarise_trades(events).count, 0u);
  EXPECT_EQ(lob.size(), 2u);  // both still resting; no clearing price
}

TEST(Auction, MarketTypeRejectsDuringCollection) {
  LimitOrderBook lob;
  lob.start_auction();
  auto events = lob.submit(make_market(1, Side::Bid, 5));
  ASSERT_EQ(events.size(), 1u);
  EXPECT_TRUE(holds<Reject>(events[0]));
}

TEST(Auction, IOCRejectsDuringCollection) {
  LimitOrderBook lob;
  lob.start_auction();
  auto events = lob.submit(make_ioc(1, Side::Bid, 100, 5));
  ASSERT_EQ(events.size(), 1u);
  EXPECT_TRUE(holds<Reject>(events[0]));
}

// ─── Market data feed (L3) ─────────────────────────────────────────────

TEST(MarketData, AddOrderOnResting) {
  LimitOrderBook lob;
  (void)lob.submit(make_limit(1, Side::Bid, 100, 5));
  const auto& md = lob.market_data_log();
  ASSERT_EQ(md.size(), 1u);
  ASSERT_TRUE(std::holds_alternative<MdAddOrder>(md[0].event));
  const auto& a = std::get<MdAddOrder>(md[0].event);
  EXPECT_EQ(a.id, 1u);
  EXPECT_EQ(a.qty.units, 5);
}

TEST(MarketData, ExecuteAndDeleteOnFullCross) {
  LimitOrderBook lob;
  (void)lob.submit(make_limit(1, Side::Ask, 100, 3));
  (void)lob.submit(make_limit(2, Side::Bid, 100, 3));

  const auto& md = lob.market_data_log();
  ASSERT_EQ(md.size(), 3u);
  EXPECT_TRUE(std::holds_alternative<MdAddOrder>(md[0].event));      // maker rest
  EXPECT_TRUE(std::holds_alternative<MdExecuteOrder>(md[1].event));   // crossed
  EXPECT_TRUE(std::holds_alternative<MdDeleteOrder>(md[2].event));    // maker fully filled
}

TEST(MarketData, DeleteOnUserCancel) {
  LimitOrderBook lob;
  (void)lob.submit(make_limit(1, Side::Bid, 100, 5));
  (void)lob.cancel(1, /*ts=*/0);
  const auto& md = lob.market_data_log();
  ASSERT_GE(md.size(), 2u);
  EXPECT_TRUE(std::holds_alternative<MdDeleteOrder>(md.back().event));
  EXPECT_EQ(std::get<MdDeleteOrder>(md.back().event).id, 1u);
}

TEST(MarketData, SequenceMonotonic) {
  LimitOrderBook lob;
  (void)lob.submit(make_limit(1, Side::Bid, 100, 5));
  (void)lob.submit(make_limit(2, Side::Ask, 102, 5));
  (void)lob.submit(make_limit(3, Side::Bid, 102, 5));  // crosses #2
  const auto& md = lob.market_data_log();
  for (std::size_t i = 0; i < md.size(); ++i) {
    EXPECT_EQ(md[i].seq, i + 1);
  }
}

TEST(MarketData, SubscriberCalledSynchronously) {
  LimitOrderBook lob;
  std::vector<MarketDataEvent> received;
  lob.subscribe_market_data([&](const MarketDataEnvelope& env){
    received.push_back(env.event);
  });
  (void)lob.submit(make_limit(1, Side::Bid, 100, 5));
  EXPECT_EQ(received.size(), 1u);
  EXPECT_TRUE(std::holds_alternative<MdAddOrder>(received[0]));
}

// ─── Self-match prevention ─────────────────────────────────────────────

namespace {
Order make_smp(OrderId id, ParticipantId p, Side side, std::int64_t price, std::int64_t qty, SmpMode m) {
  Order o = make_limit(id, side, price, qty);
  o.participant = p;
  o.smp = m;
  return o;
}
} // namespace

TEST(SMP, NoneAllowsSelfTrade) {
  LimitOrderBook lob;
  (void)lob.submit(make_smp(1, /*p=*/7, Side::Ask, 100, 5, SmpMode::None));
  auto events = lob.submit(make_smp(2, /*p=*/7, Side::Bid, 100, 3, SmpMode::None));
  // Self-trade allowed → Ack + Trade
  EXPECT_EQ(summarise_trades(events).total_qty, 3);
}

TEST(SMP, CancelNewestCancelsTakerStopsWalk) {
  LimitOrderBook lob;
  (void)lob.submit(make_smp(1, /*p=*/7, Side::Ask, 100, 5, SmpMode::None));
  auto events = lob.submit(make_smp(2, /*p=*/7, Side::Bid, 100, 3, SmpMode::CancelNewest));
  // Ack + CancelAck for taker; no trade
  ASSERT_GE(events.size(), 2u);
  EXPECT_TRUE(holds<Ack>(events[0]));
  EXPECT_TRUE(holds<CancelAck>(events[1]));
  EXPECT_EQ(summarise_trades(events).total_qty, 0);
  // Maker untouched.
  EXPECT_EQ(lob.ask_levels(1)[0].total_qty.units, 5);
}

TEST(SMP, CancelOldestCancelsMakerTakerContinues) {
  LimitOrderBook lob;
  (void)lob.submit(make_smp(1, /*p=*/7, Side::Ask, 100, 3, SmpMode::None));   // own ask
  (void)lob.submit(make_smp(2, /*p=*/8, Side::Ask, 100, 4, SmpMode::None));   // other-party ask, same level

  // Taker is participant 7 with CancelOldest: should skip own maker (1), trade with 2.
  auto events = lob.submit(make_smp(3, /*p=*/7, Side::Bid, 100, 4, SmpMode::CancelOldest));
  // Expect: Ack(taker) + CancelAck(maker 1) + Trade(maker 2)
  EXPECT_TRUE(holds<Ack>(events[0]));
  bool saw_cancel = false, saw_trade = false;
  for (auto& e : events) {
    if (std::holds_alternative<CancelAck>(e) && std::get<CancelAck>(e).id == 1u) saw_cancel = true;
    if (std::holds_alternative<Trade>(e)     && std::get<Trade>(e).maker_id == 2u) saw_trade = true;
  }
  EXPECT_TRUE(saw_cancel);
  EXPECT_TRUE(saw_trade);
  EXPECT_EQ(lob.size(), 0u);
}

TEST(SMP, CancelBothCancelsMakerAndTaker) {
  LimitOrderBook lob;
  (void)lob.submit(make_smp(1, /*p=*/7, Side::Ask, 100, 5, SmpMode::None));
  auto events = lob.submit(make_smp(2, /*p=*/7, Side::Bid, 100, 3, SmpMode::CancelBoth));
  // Ack + CancelAck(maker) + CancelAck(taker). No trade.
  bool maker_cancel = false, taker_cancel = false;
  for (auto& e : events) {
    if (std::holds_alternative<CancelAck>(e)) {
      auto& c = std::get<CancelAck>(e);
      if (c.id == 1u) maker_cancel = true;
      if (c.id == 2u) taker_cancel = true;
    }
  }
  EXPECT_TRUE(maker_cancel);
  EXPECT_TRUE(taker_cancel);
  EXPECT_EQ(summarise_trades(events).total_qty, 0);
  EXPECT_EQ(lob.size(), 0u);  // maker gone, taker not rested
}

TEST(SMP, AnonymousParticipantsDoNotTriggerSMP) {
  // p=0 means anonymous; an anonymous-vs-anonymous match is NOT self-trade.
  LimitOrderBook lob;
  (void)lob.submit(make_smp(1, /*p=*/0, Side::Ask, 100, 5, SmpMode::CancelBoth));
  auto events = lob.submit(make_smp(2, /*p=*/0, Side::Bid, 100, 3, SmpMode::CancelBoth));
  EXPECT_EQ(summarise_trades(events).total_qty, 3);
}

// ─── Pre-trade risk ────────────────────────────────────────────────────

namespace {
Order make_p_limit(OrderId id, ParticipantId p, Side side, std::int64_t price, std::int64_t qty, Timestamp ts = 0) {
  Order o = make_limit(id, side, price, qty, ts);
  o.participant = p;
  return o;
}
} // namespace

TEST(Risk, ZeroParticipantBypassesChecks) {
  // Default participant=0 means anonymous; no risk applied.
  LimitOrderBook lob;
  auto events = lob.submit(make_limit(1, Side::Bid, 100, 1'000'000));  // huge qty, no policy
  EXPECT_TRUE(holds<Ack>(events[0]));
}

TEST(Risk, KillSwitchRejectsFromParticipant) {
  LimitOrderBook lob;
  lob.set_risk_policy(/*p=*/1, LimitOrderBook::RiskPolicy{Price{0}, Quantity{0}, Quantity{0}, /*killed=*/true});
  auto events = lob.submit(make_p_limit(1, /*p=*/1, Side::Bid, 100, 1));
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(std::get<Reject>(events[0]).reason, RejectReason::RiskParticipantKilled);
}

TEST(Risk, UnkillResumesParticipant) {
  LimitOrderBook lob;
  lob.set_risk_policy(1, LimitOrderBook::RiskPolicy{Price{0}, Quantity{0}, Quantity{0}, true});
  (void)lob.submit(make_p_limit(1, 1, Side::Bid, 100, 1));  // rejected
  lob.unkill_participant(1);
  auto events = lob.submit(make_p_limit(2, 1, Side::Bid, 100, 1));
  EXPECT_TRUE(holds<Ack>(events[0]));
}

TEST(Risk, FatFingerQtyReject) {
  LimitOrderBook lob;
  lob.set_risk_policy(1, LimitOrderBook::RiskPolicy{Price{0}, /*max_qty=*/Quantity{100}, Quantity{0}, false});
  auto events = lob.submit(make_p_limit(1, 1, Side::Bid, 100, 101));
  EXPECT_EQ(std::get<Reject>(events[0]).reason, RejectReason::RiskFatFingerQty);
}

TEST(Risk, FatFingerPriceReject) {
  LimitOrderBook lob;
  lob.configure_circuit_breaker(0, 0, Price{10'000});  // sets reference_price_
  lob.set_risk_policy(1, LimitOrderBook::RiskPolicy{/*max_dev=*/Price{50}, Quantity{0}, Quantity{0}, false});

  auto good = lob.submit(make_p_limit(1, 1, Side::Bid, 10'040, 1));  // dev=40, ok
  EXPECT_TRUE(holds<Ack>(good[0]));

  auto bad = lob.submit(make_p_limit(2, 1, Side::Bid, 10'080, 1));   // dev=80, reject
  EXPECT_EQ(std::get<Reject>(bad[0]).reason, RejectReason::RiskFatFingerPrice);
}

TEST(Risk, PositionLimitRejectsBeforeBreach) {
  LimitOrderBook lob;
  lob.set_risk_policy(1, LimitOrderBook::RiskPolicy{Price{0}, Quantity{0}, /*max_pos=*/Quantity{10}, false});

  // First trade: maker P=2 sells 10, taker P=1 buys 10 → P=1 net long 10. Within limit.
  (void)lob.submit(make_p_limit(1, /*p=*/2, Side::Ask, 100, 10));
  auto fill = lob.submit(make_p_limit(2, /*p=*/1, Side::Bid, 100, 10));
  EXPECT_EQ(lob.net_position(1), 10);

  // Pre-trade check: another +1 buy would push net to 11 (> max 10) → reject.
  (void)lob.submit(make_p_limit(3, /*p=*/3, Side::Ask, 100, 5));
  auto bad = lob.submit(make_p_limit(4, /*p=*/1, Side::Bid, 100, 1));
  EXPECT_EQ(std::get<Reject>(bad[0]).reason, RejectReason::RiskPositionLimit);
  (void)fill;
}

TEST(Risk, NetPositionTracksTradesNotAcks) {
  LimitOrderBook lob;
  // Resting bid that doesn't trade.
  (void)lob.submit(make_p_limit(1, /*p=*/1, Side::Bid, 100, 5));
  EXPECT_EQ(lob.net_position(1), 0);  // no fill yet
}

// ─── Modify (cancel-replace) ───────────────────────────────────────────

TEST(Modify, UnknownIdRejects) {
  LimitOrderBook lob;
  auto events = lob.modify(/*id=*/123, Price{100}, Quantity{1}, /*ts=*/0);
  ASSERT_EQ(events.size(), 1u);
  EXPECT_TRUE(holds<Reject>(events[0]));
}

TEST(Modify, SamePriceQtyReductionKeepsPriority) {
  // Two makers at the same price. After reducing #1's qty, #1 should remain at the head.
  LimitOrderBook lob;
  (void)lob.submit(make_limit(1, Side::Ask, 100, 5));
  (void)lob.submit(make_limit(2, Side::Ask, 100, 4));

  auto events = lob.modify(1, Price{100}, Quantity{3}, /*ts=*/0);
  ASSERT_EQ(events.size(), 1u);
  ASSERT_TRUE(holds<Modified>(events[0]));
  EXPECT_EQ(std::get<Modified>(events[0]).new_qty.units, 3);

  // A taker hitting 1 unit should consume from #1 first (priority kept).
  auto t = lob.submit(make_limit(99, Side::Bid, 100, 1));
  ASSERT_GE(t.size(), 2u);
  EXPECT_EQ(std::get<Trade>(t[1]).maker_id, 1u);

  // Total resting qty at the level: (3-1) + 4 = 6
  EXPECT_EQ(lob.ask_levels(1)[0].total_qty.units, 6);
}

TEST(Modify, QtyIncreaseLosesPriority) {
  LimitOrderBook lob;
  (void)lob.submit(make_limit(1, Side::Ask, 100, 5));
  (void)lob.submit(make_limit(2, Side::Ask, 100, 4));

  auto events = lob.modify(1, Price{100}, Quantity{10}, /*ts=*/0);
  // Cancel-replace path: CancelAck + Ack (re-rested at back of queue).
  ASSERT_GE(events.size(), 2u);
  EXPECT_TRUE(holds<CancelAck>(events[0]));
  EXPECT_TRUE(holds<Ack>(events[1]));

  // Now order 2 should be at head (it kept its time priority).
  auto t = lob.submit(make_limit(99, Side::Bid, 100, 1));
  EXPECT_EQ(std::get<Trade>(t[1]).maker_id, 2u);
}

TEST(Modify, PriceChangeLosesPriority) {
  LimitOrderBook lob;
  (void)lob.submit(make_limit(1, Side::Ask, 100, 5));
  auto events = lob.modify(1, Price{102}, Quantity{5}, /*ts=*/0);
  ASSERT_GE(events.size(), 2u);
  EXPECT_TRUE(holds<CancelAck>(events[0]));
  EXPECT_TRUE(holds<Ack>(events[1]));
  EXPECT_EQ(lob.best_ask()->ticks, 102);
}

TEST(Modify, PriceChangeThatCrossesEmitsTrades) {
  LimitOrderBook lob;
  (void)lob.submit(make_limit(1, Side::Bid, 100, 5));   // resting bid
  (void)lob.submit(make_limit(2, Side::Ask, 105, 3));   // resting ask at 105

  // Modify the ask down to 100 → now crosses the bid → produces a Trade.
  auto events = lob.modify(2, Price{100}, Quantity{3}, /*ts=*/0);
  auto s = summarise_trades(events);
  EXPECT_EQ(s.total_qty, 3);
}

TEST(Modify, BelowFilledQtyRejects) {
  // Set up an order that has been partially filled, then try to modify below
  // the cumulative fill. Should reject without touching the order.
  LimitOrderBook lob;
  (void)lob.submit(make_limit(1, Side::Ask, 100, 5));
  (void)lob.submit(make_limit(2, Side::Bid, 100, 3));   // fills 3 of order 1

  // Order 1 has filled=3, qty=5, remaining=2. Modify new_qty=2 should reject.
  auto events = lob.modify(1, Price{100}, Quantity{2}, /*ts=*/0);
  ASSERT_EQ(events.size(), 1u);
  ASSERT_TRUE(holds<Reject>(events[0]));
  EXPECT_EQ(std::get<Reject>(events[0]).reason, RejectReason::ModifyBelowFilledQty);
}

// ─── Telemetry ─────────────────────────────────────────────────────────

TEST(Telemetry, CountsAcksAndTrades) {
  LimitOrderBook lob;
  (void)lob.submit(make_limit(1, Side::Ask, 100, 5));
  (void)lob.submit(make_limit(2, Side::Bid, 100, 3));  // 1 trade

  const auto& t = lob.telemetry();
  EXPECT_EQ(t.submits_accepted, 2u);
  EXPECT_EQ(t.trades, 1u);
  EXPECT_EQ(t.trade_volume, 300u);  // 100 * 3
}

TEST(Telemetry, CountsRejectsAndCancels) {
  LimitOrderBook lob;
  (void)lob.submit(make_limit(1, Side::Bid, 100, 1));
  (void)lob.submit(make_limit(1, Side::Bid, 100, 1));  // duplicate id reject
  (void)lob.cancel(1, 0);

  const auto& t = lob.telemetry();
  EXPECT_EQ(t.submits_accepted, 1u);
  EXPECT_EQ(t.submits_rejected, 1u);
  EXPECT_EQ(t.cancels_ack, 1u);
}

// ─── Audit log ─────────────────────────────────────────────────────────

TEST(AuditLog, EmptyOnFreshBook) {
  LimitOrderBook lob;
  EXPECT_TRUE(lob.audit_log().empty());
}

TEST(AuditLog, RecordsEveryEmittedEvent) {
  LimitOrderBook lob;
  (void)lob.submit(make_limit(1, Side::Ask, 100, 5));
  (void)lob.submit(make_limit(2, Side::Bid, 100, 3));  // Ack + 1 Trade
  (void)lob.cancel(1, 0);                              // CancelAck (1 has qty=2 left)

  const auto& log = lob.audit_log();
  // 1 (Ack ask) + 2 (Ack + Trade for cross) + 1 (CancelAck) = 4
  ASSERT_EQ(log.size(), 4u);
}

TEST(AuditLog, SequenceNumbersAreDenseAndMonotonic) {
  LimitOrderBook lob;
  (void)lob.submit(make_limit(1, Side::Bid, 100, 1, /*ts=*/10));
  (void)lob.submit(make_limit(2, Side::Bid, 99,  1, /*ts=*/20));
  (void)lob.submit(make_limit(3, Side::Ask, 100, 1, /*ts=*/30));  // crosses #1

  const auto& log = lob.audit_log();
  ASSERT_GE(log.size(), 4u);
  for (std::size_t i = 0; i < log.size(); ++i) {
    EXPECT_EQ(log[i].seq, i + 1);
  }
}

TEST(AuditLog, TimestampsAreNonDecreasing) {
  LimitOrderBook lob;
  (void)lob.submit(make_limit(1, Side::Bid, 100, 1, /*ts=*/100));
  (void)lob.submit(make_limit(2, Side::Bid, 100, 1, /*ts=*/200));
  (void)lob.submit(make_limit(3, Side::Bid, 100, 1, /*ts=*/50));  // client clock skew

  const auto& log = lob.audit_log();
  Timestamp prev = 0;
  for (const auto& env : log) {
    EXPECT_GE(env.ts, prev);
    prev = env.ts;
  }
}

// ─── Circuit breaker ───────────────────────────────────────────────────

TEST(CircuitBreaker, NormalWhenDisabledByDefault) {
  LimitOrderBook lob;
  (void)lob.submit(make_limit(1, Side::Ask, 100, 5));
  (void)lob.submit(make_limit(2, Side::Bid, 100, 5));
  EXPECT_EQ(lob.circuit_state(), CircuitState::Normal);
}

TEST(CircuitBreaker, WarnsOnModeratePriceMove) {
  LimitOrderBook lob;
  lob.configure_circuit_breaker(/*warn=*/0.3, /*trigger=*/0.6, Price{10'000});

  (void)lob.submit(make_limit(1, Side::Ask, 10'040, 1));  // 0.4% above ref
  (void)lob.submit(make_limit(2, Side::Bid, 10'040, 1));
  EXPECT_EQ(lob.circuit_state(), CircuitState::Warning);
}

TEST(CircuitBreaker, TriggersOnLargePriceMove) {
  LimitOrderBook lob;
  lob.configure_circuit_breaker(0.3, 0.6, Price{10'000});

  (void)lob.submit(make_limit(1, Side::Ask, 10'070, 1));  // 0.7% — triggers
  (void)lob.submit(make_limit(2, Side::Bid, 10'070, 1));
  EXPECT_EQ(lob.circuit_state(), CircuitState::Triggered);
}

TEST(CircuitBreaker, RejectsSubmitWhileTriggered) {
  LimitOrderBook lob;
  lob.configure_circuit_breaker(0.3, 0.6, Price{10'000});
  (void)lob.submit(make_limit(1, Side::Ask, 10'080, 1));
  (void)lob.submit(make_limit(2, Side::Bid, 10'080, 1));
  ASSERT_EQ(lob.circuit_state(), CircuitState::Triggered);

  auto events = lob.submit(make_limit(3, Side::Bid, 10'000, 1));
  ASSERT_EQ(events.size(), 1u);
  ASSERT_TRUE(holds<Reject>(events[0]));
  EXPECT_EQ(std::get<Reject>(events[0]).reason, RejectReason::CircuitBreakerTriggered);
}

TEST(CircuitBreaker, AllowsCancelWhileTriggered) {
  LimitOrderBook lob;
  (void)lob.submit(make_limit(1, Side::Bid, 9'990, 1));   // resting, away from action
  lob.configure_circuit_breaker(0.3, 0.6, Price{10'000});

  (void)lob.submit(make_limit(2, Side::Ask, 10'080, 1));
  (void)lob.submit(make_limit(3, Side::Bid, 10'080, 1));
  ASSERT_EQ(lob.circuit_state(), CircuitState::Triggered);

  // Cancel of pre-existing resting order must still work during halt.
  auto events = lob.cancel(1, 0);
  ASSERT_EQ(events.size(), 1u);
  EXPECT_TRUE(holds<CancelAck>(events[0]));
}

TEST(CircuitBreaker, RejectsModifyWhileTriggered) {
  LimitOrderBook lob;
  (void)lob.submit(make_limit(1, Side::Bid, 9'990, 1));
  lob.configure_circuit_breaker(0.3, 0.6, Price{10'000});

  (void)lob.submit(make_limit(2, Side::Ask, 10'080, 1));
  (void)lob.submit(make_limit(3, Side::Bid, 10'080, 1));
  ASSERT_EQ(lob.circuit_state(), CircuitState::Triggered);

  auto events = lob.modify(1, Price{9'995}, Quantity{1}, 0);
  ASSERT_EQ(events.size(), 1u);
  EXPECT_TRUE(holds<Reject>(events[0]));
  EXPECT_EQ(std::get<Reject>(events[0]).reason, RejectReason::CircuitBreakerTriggered);
}

TEST(CircuitBreaker, ResumeReturnsToNormalAndAcceptsSubmit) {
  LimitOrderBook lob;
  lob.configure_circuit_breaker(0.3, 0.6, Price{10'000});
  (void)lob.submit(make_limit(1, Side::Ask, 10'080, 1));
  (void)lob.submit(make_limit(2, Side::Bid, 10'080, 1));
  ASSERT_EQ(lob.circuit_state(), CircuitState::Triggered);

  lob.resume_after_halt();
  EXPECT_EQ(lob.circuit_state(), CircuitState::Normal);

  auto events = lob.submit(make_limit(3, Side::Bid, 10'000, 1));
  EXPECT_TRUE(holds<Ack>(events[0]));
}

TEST(CircuitBreaker, TriggeredStateSticksWithoutResume) {
  // Once triggered, a small subsequent trade should not silently de-trip.
  LimitOrderBook lob;
  lob.configure_circuit_breaker(0.3, 0.6, Price{10'000});
  (void)lob.submit(make_limit(1, Side::Ask, 10'080, 1));
  (void)lob.submit(make_limit(2, Side::Bid, 10'080, 1));
  ASSERT_EQ(lob.circuit_state(), CircuitState::Triggered);

  // Triggered → resume → then a small move shouldn't relapse to Triggered without crossing the threshold.
  lob.resume_after_halt();
  (void)lob.submit(make_limit(3, Side::Ask, 10'010, 1));
  (void)lob.submit(make_limit(4, Side::Bid, 10'010, 1));   // 0.1% — below warn
  EXPECT_EQ(lob.circuit_state(), CircuitState::Normal);
}

TEST(AuditLog, TruncatesAfterSoftCap) {
  // Generate many events to exceed the 12k truncate threshold and confirm the
  // log shrinks back to ≤ 10k while sequence numbers stay monotonic.
  LimitOrderBook lob;
  for (OrderId id = 1; id <= 12'500; ++id) {
    (void)lob.submit(make_limit(id, Side::Bid, 100, 1));
  }
  const auto& log = lob.audit_log();
  EXPECT_LE(log.size(), 10'500u);   // truncated
  EXPECT_GT(log.size(), 9'000u);    // not over-truncated
  // Seq monotonicity holds across truncation.
  for (std::size_t i = 1; i < log.size(); ++i) {
    EXPECT_EQ(log[i].seq, log[i-1].seq + 1);
  }
  // The first surviving seq should be > the number of dropped entries.
  EXPECT_GT(log.front().seq, 1'500u);
}

TEST(AuditLog, EventsMatchReturnedEvents) {
  // The events returned by submit() should be byte-identical to the events
  // appended to the log (modulo seq/ts wrappers).
  LimitOrderBook lob;
  (void)lob.submit(make_limit(1, Side::Ask, 100, 5));
  auto returned = lob.submit(make_limit(2, Side::Bid, 100, 3));

  const auto& log = lob.audit_log();
  const std::size_t n_returned = returned.size();
  ASSERT_GE(log.size(), n_returned);
  // Last n_returned entries in the log should match the returned events.
  for (std::size_t i = 0; i < n_returned; ++i) {
    EXPECT_EQ(log[log.size() - n_returned + i].event.index(),
              returned[i].index());
  }
}
