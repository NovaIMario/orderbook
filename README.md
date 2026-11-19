# Order Book Lab

**A C++20 limit order book matching engine that handles 31 million operations a second, plus a live market simulation built on it that runs in your browser.**

**▶ Live simulation: https://novaimario.github.io/orderbook/**

## The engine

A single-instrument limit order book with price-time priority:

- **Order types:** limit (GTC), market, immediate-or-cancel and fill-or-kill orders, plus cancel and amend. Reducing an order's size keeps its time priority; increasing it moves the order to the back of its price level, as on most exchanges.
- **Flat price ladder:** each side is an array of price levels indexed by tick, so finding a level is a single array access.
- **Hierarchical bitset:** one bit per tick marks each non-empty level, with a summary layer above every 64 words. When the best level empties, the next best price is found in at most `log64(ticks)` word scans, which is 3 for the default 262,144 ticks. Lookups use `std::countr_zero` and `std::countl_zero`.
- **Intrusive FIFO queues:** orders sit in a pooled vector and are doubly linked within their level. Cancelling is O(1), and once the pool is warm, adding or removing an order allocates no memory.
- **Sequential ids:** the book assigns order ids itself, so looking up an order by id is a vector index rather than a hash table probe.

### Benchmark

Pre-generated operations (63% limit, 34% cancel and 3% market orders, with a random-walk mid price and most orders placed near the touch), replayed through both books:

| Machine and compiler | `ob::OrderBook` | `std::map` reference book | Speed-up |
| --- | --- | --- | --- |
| Windows laptop, g++ 15 `-O3` (5M ops) | **31.2 M ops/s** (32 ns/op) | 7.9 M ops/s (127 ns/op) | 3.9× |
| GitHub Actions Linux, GCC (2M ops) | **31.9 M ops/s** (31 ns/op) | 13.9 M ops/s (72 ns/op) | 2.3× |
| GitHub Actions Windows, MSVC (2M ops) | **46.4 M ops/s** (22 ns/op) | 11.1 M ops/s (90 ns/op) | 4.2× |

The reference book uses `std::map` of `std::list` with an `unordered_map` index. All numbers are single threaded, and CI reruns the benchmark on every push. To reproduce: `./build/ob_bench 5000000`.

### Correctness

- 15 unit tests cover priority, partial fills, the maker-price rule, IOC/FOK, cancels in the middle of a queue, amend priority, input validation, and best-price updates across sparse levels.
- **Differential testing:** 20 seeds × 20,000 random operations are run through `OrderBook` and the deliberately simple `ReferenceBook` side by side. Order results, every trade, the best bid and ask, and 20 levels of depth must match exactly after every step, which comes to about 2 million checks.
- The bitset is also checked against a brute-force scan.
- CI builds with GCC, Clang (with AddressSanitizer and UBSan) and MSVC.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/ob_bench
```

### API

```cpp
#include "ob/order_book.hpp"

ob::OrderBook book;                                         // ticks 1..262,143
auto bid = book.limit(ob::Side::Buy, 10000, 50);            // rests: {id, Status::Resting, 0, 50}
book.limit(ob::Side::Sell, 10002, 30);
auto r = book.limit(ob::Side::Buy, 10002, 40, ob::TimeInForce::IOC);
for (const ob::Trade& t : book.trades()) { /* maker, taker, price, qty, taker_side */ }
book.amend(bid.id, 20);                                     // smaller size, keeps its place
book.cancel(bid.id);
auto top5 = book.depth(ob::Side::Sell, 5);                  // {price, qty, orders} per level
```

## The simulation

[`web/`](web) has a JavaScript port of the same matching rules, with traders who interact through it:

| Trader | Behaviour |
| --- | --- |
| Market makers | Quote both sides around the mid, lean against their inventory and against recent order flow, and widen their quotes when the price is jumping |
| Noise traders | Random small market orders, plus limit orders near the touch that expire |
| Momentum traders | Buy after the price has risen and sell after it has fallen |
| Informed traders | See the hidden fair value and trade harder the further the price is from it |

You can change the number of each trader type, add good or bad news, start a flash crash (a sell program that dumps about 12 times the visible bid depth, like the May 2010 algorithm), or trade yourself and see your market impact. The textbook results come out of the simulation:

- **With the default traders,** the mid price stays within about 0.2% of fair value. Informed traders profit and noise traders pay for it.
- **Without market makers,** the book often goes one-sided and the price drifts 5–12% away from fair value.
- **Without informed traders,** nothing pulls the price back after a crash, and it ends 20–50% below fair value.
- **With 10 momentum traders,** the rebound after a crash overshoots fair value by about 4–5%.

The web code has no dependencies or build step: `cd web && python -m http.server`, then open http://localhost:8000. Run the tests with `cd web && node --test`.

## Layout

```
include/ob/order_book.hpp      engine interface
include/ob/bitset.hpp          hierarchical bitset
include/ob/reference_book.hpp  simple std::map book for testing and benchmarking
src/order_book.cpp             engine
tests/test_order_book.cpp      unit and differential tests
bench/bench.cpp                benchmark
web/                           browser simulation (deployed to GitHub Pages)
```
