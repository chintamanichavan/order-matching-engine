# Where does my worst-case latency come from?

The question, workload, method and predictions below were written before any
measuring and are left as written. What was measured follows them, from
[Measured](#measured) on.

## The question

A matching engine's typical latency is easy to make small. What hurts a
trading firm is the slow tail: the one call in a thousand, or in a million,
that takes a hundred times longer. This study measures the engine's latency on
a realistic stream of orders, finds what causes its slowest calls, and fixes
the causes one at a time, measuring after each fix.

## The workload

A seeded stream of 2,000,000 operations after 200,000 of warm-up, around a mid
price that drifts:

| Share | Operation |
|---|---|
| 55% | a new limit order that rests, 1 to 10 ticks from the mid |
| 10% | an order that crosses the spread and trades |
| 30% | a cancel of a random resting order |
| 5%  | a modify of a random resting order |

The book holds a few thousand resting orders throughout.

## How a call is timed

`rdtscp` before and after each call, with the processor's time-stamp counter
converted to nanoseconds by timing it against the system clock. The cost of
the timing itself is measured and reported. The run is pinned to one core
(`taskset`). The CPU's power-saving frequency governor cannot be changed
without root, so results are repeated three times and the spread reported.

For every call the harness also records what happened inside it, without
touching the engine's code:

- how many heap allocations it made, and how large the largest was (a counting
  `operator new`);
- whether the audit log or market-data log was trimmed (its size fell);
- whether the audit log's storage was reallocated (its capacity changed).

## Predicted, before measuring

Reading the code gave these suspects, most likely first:

1. **Log trimming.** Both logs are capped near 10,000 entries by erasing the
   oldest 2,000 from the front of a vector. That moves about 640 KB of audit
   log (and 560 KB of market data) every 2,000 events.
2. **Exact-size reserve.** `commit_` calls `log_.reserve(size + n)`. Reserving
   an exact size defeats the vector's doubling, so until the log first reaches
   its cap, every call may copy the whole log.
3. **A result vector per call.** Every submit, cancel and modify returns a new
   `std::vector<Event>`: at least one allocation per call.
4. **Node allocation.** `std::map` levels, `std::deque` blocks and
   `std::unordered_map` nodes allocate as orders come and go; the hash map
   occasionally rehashes.
5. **Cancels scan and shift.** A cancel searches its price level's deque from
   the front and erases from the middle.

| Measure | Predicted |
|---|---|
| Typical (p50) add / cancel | 150 to 300 ns / 200 to 400 ns |
| p99.9, all calls | 2 to 5 µs |
| Worst call | 30 to 100 µs: a log trim (640 KB moved) |
| Allocations per call | 2 to 5 |
| What the slowest 0.1% of calls have in common | mostly log trims |

## The fixes, one at a time

Each is measured on its own before the next goes in. All 73 existing tests
must pass after each.

| Fix | Predicted effect |
|---|---|
| 1. Logs become fixed-size rings, allocated once | worst call falls from tens of µs to a few µs; trims disappear from the tail |
| 2. Callers pass in a reusable buffer for events | one or two fewer allocations per call; p50 down 30 to 60 ns |
| 3. Pooled memory for book levels, queues and the order index, sized at start | no allocations in steady state; p99 and p99.9 down |

## Not in scope

Multi-threading, kernel bypass, and the browser demo's speed. This is about
the engine's own single-threaded path.

---

## Measured

All runs: 2,000,000 timed calls after 200,000 of warm-up, pinned to core 3 of
an AMD Ryzen 9 9900X, `-O3`, GCC 15. Each stage was run three times; the
tables show the range over the three runs. The raw results are in
`bench/results/` (`python3 bench/summarize.py` prints them as one table). The
timing itself costs 10 to 20 ns, so nothing below about 20 ns is meaningful.

### What the slow calls had in common

Before any fix, 99.7% of the slowest 0.1% of calls (2,010 of 2,016 in the
first run) were calls in which a log was trimmed. A trim took 7.7 µs at the
median, and the single slowest call of every run was a trim (113 to 121 µs).
The prediction named the right cause.

### Each fix, measured

All calls, in nanoseconds:

| Stage | p50 | p99 | p99.9 | p99.99 | worst | allocations per call |
|---|---|---|---|---|---|---|
| Before | 80 | 430 | 6,672–7,664 | 8,856–8,997 | 112,692–120,808 | 1.80 |
| 1. Logs become fixed rings | 80 | 400–410 | 541–551 | 1,402–1,482 | 13,936–16,010 | 1.80 |
| 2. Caller's reusable event buffer | 80 | 390–400 | 520–541 | 1,382–1,452 | 15,108–22,121 | 0.63 |
| 3. Pooled memory for the book | 80 | 390–400 | 520–541 | 1,362–1,512 | 18,815–24,105 | 0.00 |
| 4. Cancel and modify go straight to the order | 90 | 220 | 310–320 | 641–661 | 4,849–24,416 | 0.00 |

Median by operation, in nanoseconds:

| Stage | add | cross | cancel | modify |
|---|---|---|---|---|
| Before | 70 | 110 | 170 | 310 |
| After fix 3 | 70 | 100 | 150 | 280 |
| After fix 4 | 90 | 100 | 80 | 150–160 |

### Predicted against measured

| Measure | Predicted | Measured | |
|---|---|---|---|
| p50 add / cancel | 150–300 / 200–400 ns | 70 / 170 ns | faster than predicted |
| p99.9, all calls | 2–5 µs | 6.7–7.7 µs | slower than predicted |
| Worst call | 30–100 µs, a log trim | 113–121 µs, a log trim | right cause, a little worse |
| Allocations per call | 2 to 5 | 1.8 | fewer than predicted |
| Slowest 0.1% in common | mostly log trims | 99.7% had a trim | right |
| Fix 1 | worst falls to a few µs; trims leave the tail | p99.9 fell 13 times, to 0.55 µs; no trims; worst fell to 14–16 µs | right about the tail, wrong about the worst (see below) |
| Fix 2 | 1 or 2 fewer allocations; p50 down 30–60 ns | 1.2 fewer allocations; p50 unchanged | allocations right, speed wrong |
| Fix 3 | no allocations in steady state; p99 and p99.9 down | no allocations; p99 and p99.9 unchanged | allocations right, speed wrong |

Fixes 2 and 3 removed every allocation from the hot path and changed no
latency number. The allocations were real, but glibc served each small one
from a per-thread cache in a few nanoseconds. Removing allocation is still
worth having (a busy allocator, or another thread using it, would change
this), but on this machine it was not where the time went.

### Fix 4, found by measuring

After fix 3, most of the slowest 0.1% were modifies and cancels, and half of
them made no allocation at all. A cancel searched its price level's queue from
the front to find the order, and a modify searched it two or three times. (The
same was already true after fix 2.) To test this before changing any code,
the stream was run, at the fix 2 stage, with new orders
spread over 100 ticks instead of 10, so the same number of orders sat in
queues about ten times shorter. Cancel p99 fell from 380 to 170 ns and p99.9
from 460 to 210 ns. The search was the cost.

Fix 4 keeps each resting order's position in its queue in the order index.
The queue became a `std::pmr::list`, whose positions stay valid as other
orders come and go, so cancel and modify go straight to the order. Cancel's
median halved, modify's nearly halved, and p99.9 for all calls fell from
about 530 to 310 ns. The price: an add is now 20 ns slower at the median (90
against 70 ns), because each order is its own list node rather than a slot in
a block of them.

Now the slowest 0.1% are mostly crossing orders (1,335 to 1,693 of about
2,100 to 2,500), which walk through several resting orders and emit a trade
for each. That is the work a trade has to do.

### The worst single call is the machine, not the engine

After fix 1 the single worst call stayed between 5 and 24 µs in every run,
whatever the fix, and landed on any kind of operation. To check whether the
engine causes it, the harness also times an engine-free busy loop (100
multiply-adds, no memory traffic) 2,000,000 times in the same way. Its median
is the same 90 ns as the engine's.

| | worst call | calls over 5 µs |
|---|---|---|
| Engine, after fix 4 | 4.8–24.4 µs | 0, 6, 7 |
| Busy loop, same runs | 13.4–14.9 µs | 3, 5, 6 |

The busy loop has the same worst case. These are interrupts and other work
by the operating system landing inside a timed call. Removing them needs an
isolated core (`isolcpus`, `nohz_full`) and a fixed CPU frequency, which need
root on this machine; it is the next step for anyone running this on their
own hardware.

### What changed in the code

- `include/rtrade/bounded_log.hpp`: a fixed-capacity ring, allocated once.
  The audit and market-data logs keep their most recent 10,000 entries,
  oldest first, and never move an entry.
- `submit`, `cancel` and `modify` have overloads that write into a caller's
  `std::vector<Event>` (cleared first). The old calls still return a new
  vector and are now wrappers.
- The book's maps, queues and order index draw memory from one
  `std::pmr::unsynchronized_pool_resource`; the index is sized for 65,536
  orders up front.
- Each price level is a `std::pmr::list`; the order index stores each order's
  place in it.
- The engine can no longer be copied: a copy's index would point into the
  original's queues. (The first throughput benchmark to copy a book after fix
  4 crashed with heap corruption; that is how this was found.)
- Building a book now costs about 14 µs, because the logs and index are
  allocated in full at start. This is paid once.

80 tests pass (the 73 from before, with the log test rewritten for the ring,
plus 7 new), also under AddressSanitizer and UndefinedBehaviorSanitizer. The
browser demo, rebuilt with `wasm/build.sh`, gives the same book step for step
as the old build over 150,000 simulator steps.
