use std::sync::Arc;
use std::time::Duration;
use axum::{extract::State, routing::{get, post}, Json, Router};
use serde::{Deserialize, Serialize};
use tokio::sync::Mutex;
use engine::{Order, OrderBook, Side};
use risk::{CircuitBreaker, CircuitState};

struct AppState {
    book:    Mutex<OrderBook>,
    circuit: Mutex<CircuitBreaker>,
}

#[derive(Deserialize)]
struct OrderRequest {
    side:        String,
    price_cents: i64,
    qty:         u64,
    participant: String,
}

#[derive(Serialize)]
struct OrderResponse {
    order_id: String,
    trades:   usize,
    circuit:  String,
}

#[derive(Serialize)]
struct BookSnapshot {
    best_bid:   Option<f64>,
    best_ask:   Option<f64>,
    spread:     Option<f64>,
    circuit:    String,
}

async fn submit_order(
    State(state): State<Arc<AppState>>,
    Json(req): Json<OrderRequest>,
) -> Json<OrderResponse> {
    let side = if req.side == "bid" { Side::Bid } else { Side::Ask };
    let order = Order::new(side, req.price_cents, req.qty, req.participant);

    let mut book = state.book.lock().await;
    let mut circuit = state.circuit.lock().await;

    // block new orders if triggered
    if circuit.state == CircuitState::Triggered {
        return Json(OrderResponse {
            order_id: order.id.to_string(),
            trades:   0,
            circuit:  "TRIGGERED".into(),
        });
    }

    let trades = book.insert(order.clone());

    // update circuit breaker with new mid price
    if let (Some(bid), Some(ask)) = (book.best_bid(), book.best_ask()) {
        let mid = (bid + ask) as f64 / 2.0 / 100.0;
        circuit.tick(mid);
    }

    let circuit_str = match circuit.state {
        CircuitState::Normal    => "NORMAL",
        CircuitState::Warning   => "WARNING",
        CircuitState::Triggered => "TRIGGERED",
    };

    Json(OrderResponse {
        order_id: order.id.to_string(),
        trades:   trades.len(),
        circuit:  circuit_str.into(),
    })
}

async fn book_snapshot(State(state): State<Arc<AppState>>) -> Json<BookSnapshot> {
    let book    = state.book.lock().await;
    let circuit = state.circuit.lock().await;

    let to_f64 = |p: i64| p as f64 / 100.0;

    Json(BookSnapshot {
        best_bid: book.best_bid().map(to_f64),
        best_ask: book.best_ask().map(to_f64),
        spread:   book.spread().map(to_f64),
        circuit:  format!("{:?}", circuit.state),
    })
}

async fn reset_circuit(State(state): State<Arc<AppState>>) -> Json<&'static str> {
    state.circuit.lock().await.reset();
    Json("ok")
}

#[tokio::main]
async fn main() {
    tracing_subscriber::fmt::init();

    let state = Arc::new(AppState {
        book:    Mutex::new(OrderBook::new()),
        circuit: Mutex::new(CircuitBreaker::new(0.6, 0.3, Duration::from_secs(10))),
    });

    let app = Router::new()
        .route("/orders",          post(submit_order))
        .route("/book",            get(book_snapshot))
        .route("/circuit/reset",   post(reset_circuit))
        .with_state(state);

    let addr = "0.0.0.0:3001";
    tracing::info!("Resilient-Trade API listening on {addr}");
    let listener = tokio::net::TcpListener::bind(addr).await.unwrap();
    axum::serve(listener, app).await.unwrap();
}
