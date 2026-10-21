// Times every call the engine makes on a realistic order stream, and records
// what happened inside each one. See docs/LATENCY.md.
//
//   taskset -c 3 ./build-release/latency [ops] [spread] > latency.json
//
// New orders rest 1 to `spread` ticks from the mid (default 10). A wider
// spread gives the same number of resting orders shorter queues per price.
//
// The harness never touches the engine's internals: allocations are counted
// by replacing the global operator new, and log trims and regrowth are read
// from the logs' public size and capacity.

#include "rtrade/lob.hpp"

#include <x86intrin.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <random>
#include <string_view>
#include <unordered_map>
#include <vector>

// ─── Allocation counting ─────────────────────────────────────────────

namespace {
std::uint64_t g_allocations = 0;
std::size_t g_largest = 0;
bool g_counting = false;
}  // namespace

void* operator new(std::size_t size) {
  if (g_counting) {
    ++g_allocations;
    g_largest = std::max(g_largest, size);
  }
  if (void* p = std::malloc(size == 0 ? 1 : size)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

using namespace rtrade;

// ─── Timing ──────────────────────────────────────────────────────────

inline std::uint64_t ticks() {
  unsigned aux;
  _mm_lfence();
  const std::uint64_t t = __rdtscp(&aux);
  _mm_lfence();
  return t;
}

// Time-stamp counter ticks per nanosecond, measured against the system clock.
double ticks_per_ns() {
  using clock = std::chrono::steady_clock;
  const auto c0 = clock::now();
  const std::uint64_t t0 = ticks();
  while (clock::now() - c0 < std::chrono::milliseconds(300)) {
  }
  const auto c1 = clock::now();
  const std::uint64_t t1 = ticks();
  return static_cast<double>(t1 - t0) / static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(c1 - c0).count());
}

// ─── The order stream ────────────────────────────────────────────────

enum class Op : std::uint8_t { Add, Cross, Cancel, Modify };
constexpr std::array<std::string_view, 4> kOpNames{"add", "cross", "cancel", "modify"};

struct Record {
  std::uint32_t ns;          // latency, rounded down
  Op op;
  std::uint8_t allocations;  // capped at 255
  bool trimmed;              // a log shrank during this call
  bool regrew;               // the audit log's storage was reallocated
  std::uint32_t largest;     // largest single allocation, bytes
  std::uint16_t trades;
};

class Stream {
 public:
  Stream(std::uint64_t seed, std::int64_t spread) : rng_(seed), spread_(spread) {}

  // Plays one operation on `book`, timed. `record` is null during warm-up.
  void step(LimitOrderBook& book, Record* record, double per_ns) {
    // The mid drifts slowly, so price levels come and go.
    if (pick_(rng_) < 0.01) mid_ += (pick_(rng_) < 0.5 ? -1 : 1);

    const double u = pick_(rng_);
    Op op = u < 0.55 ? Op::Add : u < 0.65 ? Op::Cross : u < 0.95 ? Op::Cancel : Op::Modify;
    // Hold the book near its target size, as a steady market would: past it,
    // a new order becomes a cancel; below it, a cancel becomes a new order.
    if (op == Op::Add && live_.size() > kTargetResting) op = Op::Cancel;
    if ((op == Op::Cancel || op == Op::Modify) && live_.size() < kTargetResting / 2) op = Op::Add;

    const std::size_t audit_size = book.audit_log().size();
    const std::size_t md_size = book.market_data_log().size();
    const std::size_t audit_capacity = book.audit_log().capacity();
    g_allocations = 0;
    g_largest = 0;

    std::uint64_t start = 0, end = 0;
    switch (op) {
      case Op::Add: {
        const Side side = pick_(rng_) < 0.5 ? Side::Bid : Side::Ask;
        const std::int64_t away = 1 + static_cast<std::int64_t>(pick_(rng_) * static_cast<double>(spread_));
        const Price price{side == Side::Bid ? mid_ - away : mid_ + away};
        const Order o{next_id_, side, OrderType::Limit, price, Quantity{1 + static_cast<std::int64_t>(pick_(rng_) * 10)}, Quantity{0}, now_++, 0, SmpMode::None};
        g_counting = true;
        start = ticks();
        book.submit(o, events_);
        end = ticks();
        g_counting = false;
        add_live_(next_id_++);
        break;
      }
      case Op::Cross: {
        const Side side = pick_(rng_) < 0.5 ? Side::Bid : Side::Ask;
        const Order o{next_id_++, side, OrderType::IOC, Price{side == Side::Bid ? mid_ + 3 : mid_ - 3}, Quantity{1 + static_cast<std::int64_t>(pick_(rng_) * 20)}, Quantity{0}, now_++, 0, SmpMode::None};
        g_counting = true;
        start = ticks();
        book.submit(o, events_);
        end = ticks();
        g_counting = false;
        break;
      }
      case Op::Cancel: {
        const OrderId id = live_[static_cast<std::size_t>(pick_(rng_) * static_cast<double>(live_.size()))];
        remove_live_(id);
        g_counting = true;
        start = ticks();
        book.cancel(id, now_++, events_);
        end = ticks();
        g_counting = false;
        break;
      }
      case Op::Modify: {
        const OrderId id = live_[static_cast<std::size_t>(pick_(rng_) * static_cast<double>(live_.size()))];
        const Side side = pick_(rng_) < 0.5 ? Side::Bid : Side::Ask;
        const Price price{side == Side::Bid ? mid_ - 2 : mid_ + 2};
        g_counting = true;
        start = ticks();
        book.modify(id, price, Quantity{1 + static_cast<std::int64_t>(pick_(rng_) * 5)}, now_++, events_);
        end = ticks();
        g_counting = false;
        break;
      }
    }

    // Orders that traded away no longer rest: keep the live list true, so
    // cancels and modifies always aim at a real resting order.
    std::uint16_t trades = 0;
    for (const Event& e : events_) {
      if (const auto* t = std::get_if<Trade>(&e)) {
        ++trades;
        remove_live_(t->maker_id);
      } else if (const auto* r = std::get_if<Reject>(&e)) {
        remove_live_(r->id);
      }
    }
    // A modify keeps its id, so it stays in the live list unless it traded
    // away or was rejected, both handled above.
    if (!record) return;
    *record = Record{
        .ns = static_cast<std::uint32_t>(static_cast<double>(end - start) / per_ns),
        .op = op,
        .allocations = static_cast<std::uint8_t>(std::min<std::uint64_t>(g_allocations, 255)),
        .trimmed = book.audit_log().size() < audit_size || book.market_data_log().size() < md_size,
        .regrew = book.audit_log().capacity() != audit_capacity,
        .largest = static_cast<std::uint32_t>(std::min<std::size_t>(g_largest, UINT32_MAX)),
        .trades = trades,
    };
  }

  std::size_t live() const { return live_.size(); }

 private:
  static constexpr std::size_t kTargetResting = 5'000;

  void add_live_(OrderId id) {
    if (where_.contains(id)) return;
    where_[id] = live_.size();
    live_.push_back(id);
  }
  void remove_live_(OrderId id) {
    const auto it = where_.find(id);
    if (it == where_.end()) return;
    const std::size_t at = it->second;
    where_.erase(it);
    if (at + 1 != live_.size()) {
      live_[at] = live_.back();
      where_[live_[at]] = at;
    }
    live_.pop_back();
  }

  std::mt19937_64 rng_;
  std::uniform_real_distribution<double> pick_{0.0, 1.0};
  std::int64_t mid_ = 10'000;
  OrderId next_id_ = 1;
  Timestamp now_ = 1;
  std::vector<OrderId> live_;  // ids resting in the book, for picking at random
  std::int64_t spread_;
  std::vector<Event> events_;  // one buffer reused by every call, as a real caller would
  std::unordered_map<OrderId, std::size_t> where_;  // id -> index in live_
};

// ─── Summaries ───────────────────────────────────────────────────────

std::uint32_t percentile(std::vector<std::uint32_t>& sorted, double q) {
  if (sorted.empty()) return 0;
  const std::size_t at = std::min(sorted.size() - 1, static_cast<std::size_t>(q * static_cast<double>(sorted.size())));
  return sorted[at];
}

void print_percentiles(const char* name, std::vector<std::uint32_t> ns, bool last) {
  std::sort(ns.begin(), ns.end());
  std::printf("    \"%s\": {\"calls\": %zu, \"p50\": %u, \"p90\": %u, \"p99\": %u, \"p99_9\": %u, \"p99_99\": %u, \"max\": %u}%s\n", name, ns.size(),
              percentile(ns, 0.5), percentile(ns, 0.9), percentile(ns, 0.99), percentile(ns, 0.999), percentile(ns, 0.9999), ns.empty() ? 0 : ns.back(), last ? "" : ",");
}

// A fixed piece of work with no memory traffic, about as long as a typical
// engine call. Timed the same way, its slow tail is the machine's own noise
// (interrupts, frequency changes), the floor under the engine's worst calls.
volatile std::uint64_t g_seed = 1;
std::uint64_t busy() {
  std::uint64_t x = g_seed;
  for (int i = 0; i < 100; ++i) x = x * 6364136223846793005ULL + 1442695040888963407ULL;
  return x;
}

std::uint64_t count_over(const std::vector<std::uint32_t>& ns, std::uint32_t limit) {
  return static_cast<std::uint64_t>(std::count_if(ns.begin(), ns.end(), [limit](std::uint32_t v) { return v > limit; }));
}

}  // namespace

int main(int argc, char** argv) {
  const std::size_t ops = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 2'000'000;
  const std::int64_t spread = argc > 2 ? std::strtoll(argv[2], nullptr, 10) : 10;
  const std::size_t warmup = 200'000;
  const double per_ns = ticks_per_ns();

  // The cost of timing nothing: the floor under every number below.
  std::vector<std::uint32_t> empty(100'000);
  for (auto& e : empty) {
    const std::uint64_t a = ticks();
    const std::uint64_t b = ticks();
    e = static_cast<std::uint32_t>(static_cast<double>(b - a) / per_ns);
  }
  std::sort(empty.begin(), empty.end());

  LimitOrderBook book;
  Stream stream(42, spread);
  for (std::size_t i = 0; i < warmup; ++i) stream.step(book, nullptr, per_ns);

  std::vector<Record> records(ops);
  for (std::size_t i = 0; i < ops; ++i) stream.step(book, &records[i], per_ns);

  std::vector<std::uint32_t> control(ops);
  for (auto& c : control) {
    const std::uint64_t a = ticks();
    g_seed = busy();
    const std::uint64_t b = ticks();
    c = static_cast<std::uint32_t>(static_cast<double>(b - a) / per_ns);
  }

  std::array<std::vector<std::uint32_t>, 4> by_op;
  std::vector<std::uint32_t> all;
  all.reserve(ops);
  std::uint64_t allocations = 0, trims = 0, regrowths = 0;
  for (const Record& r : records) {
    by_op[static_cast<std::size_t>(r.op)].push_back(r.ns);
    all.push_back(r.ns);
    allocations += r.allocations;
    trims += r.trimmed;
    regrowths += r.regrew;
  }

  // What the slowest 0.1% of calls have in common.
  std::vector<std::uint32_t> sorted = all;
  std::sort(sorted.begin(), sorted.end());
  const std::uint32_t tail = percentile(sorted, 0.999);
  std::uint64_t in_tail = 0, tail_trims = 0, tail_regrew = 0, tail_big_alloc = 0, tail_any_alloc = 0, tail_trades = 0;
  std::array<std::uint64_t, 4> tail_ops{};
  for (const Record& r : records) {
    if (r.ns < tail) continue;
    ++in_tail;
    tail_trims += r.trimmed;
    tail_regrew += r.regrew;
    tail_big_alloc += r.largest >= 16 * 1024;
    tail_any_alloc += r.allocations > 0;
    tail_trades += r.trades > 0;
    ++tail_ops[static_cast<std::size_t>(r.op)];
  }
  // How slow a trim is when it happens.
  std::vector<std::uint32_t> trim_ns;
  for (const Record& r : records) if (r.trimmed) trim_ns.push_back(r.ns);
  std::sort(trim_ns.begin(), trim_ns.end());

  std::printf("{\n  \"ops\": %zu, \"warmup\": %zu, \"resting_at_end\": %zu, \"ticks_per_ns\": %.4f,\n", ops, warmup, book.size(), per_ns);
  std::printf("  \"timing_floor_ns\": {\"p50\": %u, \"p99\": %u},\n", percentile(empty, 0.5), percentile(empty, 0.99));
  std::printf("  \"allocations_per_call\": %.3f, \"trims\": %" PRIu64 ", \"regrowths\": %" PRIu64 ",\n", static_cast<double>(allocations) / static_cast<double>(ops), trims, regrowths);
  std::printf("  \"trim_ns\": {\"p50\": %u, \"max\": %u},\n", percentile(trim_ns, 0.5), trim_ns.empty() ? 0 : trim_ns.back());
  std::printf("  \"latency_ns\": {\n");
  for (std::size_t op = 0; op < 4; ++op) print_percentiles(kOpNames[op].data(), by_op[op], false);
  print_percentiles("all", all, true);
  std::printf("  },\n");
  std::printf("  \"control_ns\": {\n");
  print_percentiles("busy_loop", control, true);
  std::printf("  },\n");
  std::printf("  \"calls_over_5us\": {\"engine\": %" PRIu64 ", \"busy_loop\": %" PRIu64 "},\n", count_over(all, 5'000), count_over(control, 5'000));
  std::printf("  \"slowest_0_1_percent\": {\"threshold_ns\": %u, \"calls\": %" PRIu64 ", \"with_trim\": %" PRIu64 ", \"with_regrowth\": %" PRIu64
              ", \"with_allocation_over_16kb\": %" PRIu64 ", \"with_any_allocation\": %" PRIu64 ", \"with_trades\": %" PRIu64
              ", \"add\": %" PRIu64 ", \"cross\": %" PRIu64 ", \"cancel\": %" PRIu64 ", \"modify\": %" PRIu64 "}\n}\n",
              tail, in_tail, tail_trims, tail_regrew, tail_big_alloc, tail_any_alloc, tail_trades, tail_ops[0], tail_ops[1], tail_ops[2], tail_ops[3]);
  return 0;
}
