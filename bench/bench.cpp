// Replays the same pre-generated order flow through OrderBook and ReferenceBook and reports
// throughput and per-operation latency percentiles.
//
//   ob_bench [operations=5000000] [seed=42]

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "ob/order_book.hpp"
#include "ob/reference_book.hpp"

using namespace ob;
using Clock = std::chrono::steady_clock;

struct Op {
  enum Kind : std::uint8_t { Limit, Market, Cancel } kind;
  Side side;
  Price price;
  Qty qty;
  OrderId id;
};

// Generate a realistic-looking flow: the mid price random-walks; most new orders are passive and
// land within a few ticks of the touch; about a third of messages cancel a live order; a few
// percent are aggressive. Ids are assigned deterministically by the book, so the stream can be
// generated against one book and replayed exactly against another.
static std::vector<Op> generate(std::size_t n, std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::geometric_distribution<int> depth(0.25);
  OrderBook book;
  std::vector<OrderId> live;
  std::vector<Op> ops;
  ops.reserve(n);
  Price mid = 100000;
  for (std::size_t i = 0; i < n; ++i) {
    if (i % 100 == 0) mid += static_cast<Price>(rng() % 5) - 2;
    const int r = static_cast<int>(rng() % 100);
    const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
    Op op{};
    if (r < 60 || live.empty()) {
      const Price off = 1 + depth(rng);
      op = {Op::Limit, side, side == Side::Buy ? mid - off : mid + off, 1 + static_cast<Qty>(rng() % 100), 0};
      if (auto res = book.limit(op.side, op.price, op.qty); res.resting) live.push_back(res.id);
    } else if (r < 94) {
      // Cancel a random live order (skipping any that have since been filled).
      while (!live.empty()) {
        const std::size_t k = rng() % live.size();
        const OrderId id = live[k];
        live[k] = live.back();
        live.pop_back();
        if (book.contains(id)) { op = {Op::Cancel, side, 0, 0, id}; book.cancel(id); break; }
      }
      if (op.id == 0) { --i; continue; }
    } else if (r < 97) {
      op = {Op::Market, side, 0, 1 + static_cast<Qty>(rng() % 300), 0};
      book.market(op.side, op.qty);
    } else {
      const Price through = static_cast<Price>(rng() % 4);
      op = {Op::Limit, side, side == Side::Buy ? mid + through : mid - through, 1 + static_cast<Qty>(rng() % 200), 0};
      if (auto res = book.limit(op.side, op.price, op.qty); res.resting) live.push_back(res.id);
    }
    ops.push_back(op);
  }
  return ops;
}

template <class Book>
static void run(const char* name, const std::vector<Op>& ops, bool sample_latency) {
  Book book;
  std::vector<std::uint32_t> lat;
  if (sample_latency) lat.reserve(ops.size());
  std::size_t trades = 0;
  const auto t0 = Clock::now();
  for (const Op& op : ops) {
    const auto s = sample_latency ? Clock::now() : Clock::time_point{};
    switch (op.kind) {
      case Op::Limit: book.limit(op.side, op.price, op.qty); trades += book.trades().size(); break;
      case Op::Market: book.market(op.side, op.qty); trades += book.trades().size(); break;
      case Op::Cancel: book.cancel(op.id); break;
    }
    if (sample_latency) lat.push_back(static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - s).count()));
  }
  const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
  if (!sample_latency) {
    std::printf("%-15s %8.2f M ops/s  %7.1f ns/op  (%zu trades, %zu resting at end)\n", name,
                ops.size() / secs / 1e6, secs * 1e9 / ops.size(), trades, book.order_count());
    return;
  }
  std::sort(lat.begin(), lat.end());
  auto pct = [&](double p) { return lat[std::min(lat.size() - 1, static_cast<std::size_t>(p * lat.size()))]; };
  std::printf("%-15s latency incl. clock overhead: p50 %u ns  p90 %u ns  p99 %u ns  p99.9 %u ns  max %u ns\n", name,
              pct(0.5), pct(0.9), pct(0.99), pct(0.999), lat.back());
}

int main(int argc, char** argv) {
  const std::size_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 5'000'000;
  const std::uint64_t seed = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 42;
  std::printf("Generating %zu operations (seed %llu)...\n", n, static_cast<unsigned long long>(seed));
  const auto ops = generate(n, seed);
  std::size_t lim = 0, mkt = 0, can = 0;
  for (const Op& o : ops) (o.kind == Op::Limit ? lim : o.kind == Op::Market ? mkt : can)++;
  std::printf("Mix: %.1f%% limit, %.1f%% cancel, %.1f%% market\n\n", 100.0 * lim / n, 100.0 * can / n, 100.0 * mkt / n);
  run<OrderBook>("OrderBook", ops, false);
  run<ReferenceBook>("std::map book", ops, false);
  std::printf("\n");
  run<OrderBook>("OrderBook", ops, true);
  run<ReferenceBook>("std::map book", ops, true);
}
