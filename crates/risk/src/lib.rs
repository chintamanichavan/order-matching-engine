use std::collections::VecDeque;
use std::time::{Duration, SystemTime};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CircuitState { Normal, Warning, Triggered }

/// NYSE Rule 80B-inspired circuit breaker.
/// Triggers when price moves more than `threshold_pct` within `window`.
pub struct CircuitBreaker {
    threshold_pct: f64,
    warn_pct:      f64,
    window:        Duration,
    price_history: VecDeque<(SystemTime, f64)>,
    pub state:     CircuitState,
}

impl CircuitBreaker {
    pub fn new(threshold_pct: f64, warn_pct: f64, window: Duration) -> Self {
        Self {
            threshold_pct,
            warn_pct,
            window,
            price_history: VecDeque::new(),
            state: CircuitState::Normal,
        }
    }

    /// Feed a new mid-price. Returns the new circuit state.
    pub fn tick(&mut self, price: f64) -> CircuitState {
        if self.state == CircuitState::Triggered { return self.state; }

        let now = SystemTime::now();
        self.price_history.push_back((now, price));

        // evict prices outside the window
        let cutoff = now - self.window;
        while self.price_history.front().map_or(false, |(t, _)| *t < cutoff) {
            self.price_history.pop_front();
        }

        let oldest = match self.price_history.front() {
            Some((_, p)) => *p,
            None => return self.state,
        };

        let pct_move = ((price - oldest) / oldest).abs() * 100.0;

        self.state = if pct_move >= self.threshold_pct {
            CircuitState::Triggered
        } else if pct_move >= self.warn_pct {
            CircuitState::Warning
        } else {
            CircuitState::Normal
        };

        self.state
    }

    pub fn reset(&mut self) {
        self.price_history.clear();
        self.state = CircuitState::Normal;
    }
}
