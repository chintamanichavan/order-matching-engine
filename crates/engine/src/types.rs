use serde::{Deserialize, Serialize};
use std::time::SystemTime;
use uuid::Uuid;

pub type OrderId = Uuid;
pub type Price   = i64;   // fixed-point: cents × 100  (e.g. $42,150.25 → 4_215_025)
pub type Qty     = u64;   // fixed-point: satoshis × 10^8

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum Side { Bid, Ask }

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Order {
    pub id:         OrderId,
    pub side:       Side,
    pub price:      Price,
    pub qty:        Qty,
    pub remaining:  Qty,
    pub participant: String,
    pub submitted_at: SystemTime,
}

impl Order {
    pub fn new(side: Side, price: Price, qty: Qty, participant: impl Into<String>) -> Self {
        Self {
            id: Uuid::new_v4(),
            side,
            price,
            qty,
            remaining: qty,
            participant: participant.into(),
            submitted_at: SystemTime::now(),
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Trade {
    pub id:          Uuid,
    pub bid_order:   OrderId,
    pub ask_order:   OrderId,
    pub price:       Price,
    pub qty:         Qty,
    pub executed_at: SystemTime,
}

impl Trade {
    pub fn new(bid: OrderId, ask: OrderId, price: Price, qty: Qty) -> Self {
        Self {
            id: Uuid::new_v4(),
            bid_order: bid,
            ask_order: ask,
            price,
            qty,
            executed_at: SystemTime::now(),
        }
    }
}
