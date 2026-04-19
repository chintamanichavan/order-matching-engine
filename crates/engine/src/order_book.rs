use std::collections::BTreeMap;
use crate::types::{Order, OrderId, Price, Side, Trade};

/// Price-time priority Limit Order Book.
///
/// Bids: BTreeMap<Price, VecDeque<Order>> — highest price first (negated key)
/// Asks: BTreeMap<Price, VecDeque<Order>> — lowest price first
///
/// All operations are O(log n) on the price level count.
pub struct OrderBook {
    bids: BTreeMap<Price, std::collections::VecDeque<Order>>,
    asks: BTreeMap<Price, std::collections::VecDeque<Order>>,
    index: std::collections::HashMap<OrderId, (Side, Price)>,
}

impl OrderBook {
    pub fn new() -> Self {
        Self {
            bids:  BTreeMap::new(),
            asks:  BTreeMap::new(),
            index: std::collections::HashMap::new(),
        }
    }

    /// Insert an order and return any trades produced by immediate matching.
    pub fn insert(&mut self, mut order: Order) -> Vec<Trade> {
        let mut trades = Vec::new();

        match order.side {
            Side::Bid => {
                // match against resting asks (lowest ask first)
                while order.remaining > 0 {
                    let Some((&ask_price, _)) = self.asks.iter().next() else { break };
                    if ask_price > order.price { break }

                    let level = self.asks.get_mut(&ask_price).unwrap();
                    let resting = level.front_mut().unwrap();

                    let fill = order.remaining.min(resting.remaining);
                    trades.push(Trade::new(order.id, resting.id, ask_price, fill));
                    order.remaining    -= fill;
                    resting.remaining  -= fill;

                    if resting.remaining == 0 {
                        let id = resting.id;
                        level.pop_front();
                        self.index.remove(&id);
                        if level.is_empty() { self.asks.remove(&ask_price); }
                    }
                }
                // rest any unfilled quantity
                if order.remaining > 0 {
                    self.index.insert(order.id, (Side::Bid, order.price));
                    self.bids
                        .entry(-order.price)   // negate for descending order
                        .or_default()
                        .push_back(order);
                }
            }
            Side::Ask => {
                while order.remaining > 0 {
                    let Some((&neg_bid_price, _)) = self.bids.iter().next() else { break };
                    let bid_price = -neg_bid_price;
                    if bid_price < order.price { break }

                    let level = self.bids.get_mut(&neg_bid_price).unwrap();
                    let resting = level.front_mut().unwrap();

                    let fill = order.remaining.min(resting.remaining);
                    trades.push(Trade::new(resting.id, order.id, bid_price, fill));
                    order.remaining   -= fill;
                    resting.remaining -= fill;

                    if resting.remaining == 0 {
                        let id = resting.id;
                        level.pop_front();
                        self.index.remove(&id);
                        if level.is_empty() { self.bids.remove(&neg_bid_price); }
                    }
                }
                if order.remaining > 0 {
                    self.index.insert(order.id, (Side::Ask, order.price));
                    self.asks.entry(order.price).or_default().push_back(order);
                }
            }
        }

        trades
    }

    pub fn cancel(&mut self, id: OrderId) -> bool {
        let Some((side, price)) = self.index.remove(&id) else { return false };
        let key = if side == Side::Bid { -price } else { price };
        let book = if side == Side::Bid { &mut self.bids } else { &mut self.asks };
        if let Some(level) = book.get_mut(&key) {
            level.retain(|o| o.id != id);
            if level.is_empty() { book.remove(&key); }
        }
        true
    }

    pub fn best_bid(&self) -> Option<Price> {
        self.bids.keys().next().map(|k| -k)
    }

    pub fn best_ask(&self) -> Option<Price> {
        self.asks.keys().next().copied()
    }

    pub fn spread(&self) -> Option<Price> {
        Some(self.best_ask()? - self.best_bid()?)
    }
}

impl Default for OrderBook { fn default() -> Self { Self::new() } }
