// Unit tests and a randomised differential test against ReferenceBook. No test framework needed.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "ob/bitset.hpp"
#include "ob/order_book.hpp"
#include "ob/reference_book.hpp"

using namespace ob;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond)                                                              \
  do {                                                                           \
    ++g_checks;                                                                  \
    if (!(cond)) { ++g_failed; std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
  } while (0)

struct TestCase { const char* name; std::function<void()> fn; };
static std::vector<TestCase>& registry() { static std::vector<TestCase> r; return r; }
struct Reg { Reg(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); } };
#define TEST(name) static void name(); static Reg reg_##name(#name, name); static void name()

TEST(bitset_next_prev) {
  HierBitset b(300000);
  CHECK(b.next(0) == HierBitset::npos);
  CHECK(b.prev(299999) == HierBitset::npos);
  for (std::int64_t i : {5, 64, 4095, 4096, 262143, 299999}) b.set(i);
  CHECK(b.next(0) == 5);
  CHECK(b.next(6) == 64);
  CHECK(b.next(65) == 4095);
  CHECK(b.next(4097) == 262143);
  CHECK(b.next(262144) == 299999);
  CHECK(b.prev(299998) == 262143);
  CHECK(b.prev(4095) == 4095);
  CHECK(b.prev(63) == 5);
  CHECK(b.prev(4) == HierBitset::npos);
  b.clear(4096);
  b.clear(4095);
  CHECK(b.next(65) == 262143);
  CHECK(b.prev(262142) == 64);
}

TEST(bitset_matches_brute_force) {
  std::mt19937_64 rng(1);
  HierBitset b(10000);
  std::vector<bool> ref(10000);
  for (int step = 0; step < 50000; ++step) {
    const std::int64_t i = rng() % 10000;
    if (rng() & 1) { b.set(i); ref[i] = true; } else { b.clear(i); ref[i] = false; }
    const std::int64_t q = rng() % 10000;
    std::int64_t n = q; while (n < 10000 && !ref[n]) ++n;
    std::int64_t p = q; while (p >= 0 && !ref[p]) --p;
    CHECK(b.next(q) == (n == 10000 ? HierBitset::npos : n));
    CHECK(b.prev(q) == (p < 0 ? HierBitset::npos : p));
  }
}

TEST(empty_book) {
  OrderBook b;
  CHECK(!b.has_bid());
  CHECK(!b.has_ask());
  CHECK(b.order_count() == 0);
  CHECK(b.depth(Side::Buy, 5).empty());
}

TEST(rests_without_crossing) {
  OrderBook b;
  auto r1 = b.limit(Side::Buy, 100, 10);
  auto r2 = b.limit(Side::Sell, 105, 7);
  CHECK(r1.status == Status::Resting && r1.resting == 10);
  CHECK(r2.status == Status::Resting);
  CHECK(b.best_bid() == 100 && b.best_ask() == 105);
  CHECK(b.trades().empty());
  CHECK(b.order_count() == 2);
}

TEST(price_time_priority) {
  OrderBook b;
  auto a = b.limit(Side::Sell, 101, 5);
  auto c = b.limit(Side::Sell, 100, 5);  // better price, later
  auto d = b.limit(Side::Sell, 100, 5);  // same price, even later
  auto t = b.limit(Side::Buy, 101, 12);
  CHECK(t.status == Status::Filled && t.filled == 12);
  const auto& tr = b.trades();
  CHECK(tr.size() == 3);
  CHECK(tr[0].maker == c.id && tr[0].price == 100 && tr[0].qty == 5);
  CHECK(tr[1].maker == d.id && tr[1].price == 100 && tr[1].qty == 5);
  CHECK(tr[2].maker == a.id && tr[2].price == 101 && tr[2].qty == 2);
  CHECK(b.volume_at(Side::Sell, 101) == 3);
  CHECK(b.best_ask() == 101);
}

TEST(trades_at_maker_price) {
  OrderBook b;
  b.limit(Side::Buy, 100, 5);
  b.limit(Side::Sell, 90, 5);
  CHECK(b.trades().size() == 1 && b.trades()[0].price == 100);
}

TEST(partial_fill_then_rest) {
  OrderBook b;
  b.limit(Side::Sell, 100, 4);
  auto r = b.limit(Side::Buy, 100, 10);
  CHECK(r.status == Status::PartiallyFilled && r.filled == 4 && r.resting == 6);
  CHECK(b.best_bid() == 100 && !b.has_ask());
}

TEST(market_orders) {
  OrderBook b;
  b.limit(Side::Sell, 100, 3);
  b.limit(Side::Sell, 200, 3);
  auto r = b.market(Side::Buy, 10);
  CHECK(r.status == Status::PartiallyFilled && r.filled == 6 && r.resting == 0);
  CHECK(!b.has_ask());
  CHECK(!b.has_bid());  // market orders never rest
  CHECK(b.market(Side::Sell, 5).status == Status::Cancelled);
}

TEST(ioc_and_fok) {
  OrderBook b;
  b.limit(Side::Sell, 100, 5);
  b.limit(Side::Sell, 102, 5);
  auto ioc = b.limit(Side::Buy, 101, 8, TimeInForce::IOC);
  CHECK(ioc.status == Status::PartiallyFilled && ioc.filled == 5 && ioc.resting == 0);
  CHECK(!b.has_bid());
  auto fok_fail = b.limit(Side::Buy, 102, 6, TimeInForce::FOK);
  CHECK(fok_fail.status == Status::Cancelled && b.trades().empty());
  CHECK(b.volume_at(Side::Sell, 102) == 5);
  auto fok_ok = b.limit(Side::Buy, 102, 5, TimeInForce::FOK);
  CHECK(fok_ok.status == Status::Filled);
  CHECK(!b.has_ask());
}

TEST(cancel) {
  OrderBook b;
  auto x = b.limit(Side::Buy, 100, 5);
  auto y = b.limit(Side::Buy, 99, 5);
  CHECK(b.cancel(x.id));
  CHECK(!b.cancel(x.id));
  CHECK(!b.cancel(12345));
  CHECK(b.best_bid() == 99);
  CHECK(b.cancel(y.id));
  CHECK(!b.has_bid());
  CHECK(b.order_count() == 0);
}

TEST(cancel_middle_of_queue) {
  OrderBook b;
  auto a = b.limit(Side::Sell, 100, 1);
  auto m = b.limit(Side::Sell, 100, 1);
  auto z = b.limit(Side::Sell, 100, 1);
  CHECK(b.cancel(m.id));
  b.market(Side::Buy, 2);
  CHECK(b.trades().size() == 2 && b.trades()[0].maker == a.id && b.trades()[1].maker == z.id);
}

TEST(amend_keeps_or_loses_priority) {
  OrderBook b;
  auto a = b.limit(Side::Sell, 100, 5);
  auto c = b.limit(Side::Sell, 100, 5);
  CHECK(b.amend(a.id, 3));  // smaller: keeps its place
  b.market(Side::Buy, 1);
  CHECK(b.trades()[0].maker == a.id);
  CHECK(b.amend(a.id, 10));  // bigger: goes to the back
  b.market(Side::Buy, 1);
  CHECK(b.trades()[0].maker == c.id);
  CHECK(b.volume_at(Side::Sell, 100) == 14);
  CHECK(b.amend(c.id, 0));
  CHECK(!b.contains(c.id));
}

TEST(rejects_bad_input) {
  OrderBook b(1000);
  CHECK(b.limit(Side::Buy, 0, 5).status == Status::Rejected);
  CHECK(b.limit(Side::Buy, 1000, 5).status == Status::Rejected);
  CHECK(b.limit(Side::Buy, 10, 0).status == Status::Rejected);
  CHECK(b.market(Side::Buy, -1).status == Status::Rejected);
}

TEST(best_price_updates_across_sparse_levels) {
  OrderBook b;
  b.limit(Side::Sell, 10, 1);
  b.limit(Side::Sell, 200000, 1);
  b.limit(Side::Buy, 5, 1);
  b.limit(Side::Buy, 1, 1);
  b.market(Side::Buy, 1);
  CHECK(b.best_ask() == 200000);
  b.market(Side::Sell, 1);
  CHECK(b.best_bid() == 1);
  auto d = b.depth(Side::Sell, 10);
  CHECK(d.size() == 1 && d[0].price == 200000);
}

// Drive both books with the same random order flow and require identical results, trades and
// depth after every step.
TEST(randomised_against_reference) {
  for (std::uint64_t seed = 1; seed <= 20; ++seed) {
    std::mt19937_64 rng(seed);
    OrderBook fast(4096);
    ReferenceBook ref(4096);
    std::vector<OrderId> ids;
    Price mid = 2000;
    for (int step = 0; step < 20000; ++step) {
      if (step % 50 == 0) mid = std::clamp<Price>(mid + static_cast<Price>(rng() % 21) - 10, 200, 3800);
      const int kind = rng() % 100;
      const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
      const Qty qty = 1 + rng() % 50;
      if (kind < 55) {
        const Price off = static_cast<Price>(rng() % 30) - 5;
        const Price px = side == Side::Buy ? mid - off : mid + off;
        const TimeInForce tif = kind < 49 ? TimeInForce::GTC : kind < 52 ? TimeInForce::IOC : TimeInForce::FOK;
        auto a = fast.limit(side, px, qty, tif);
        auto b = ref.limit(side, px, qty, tif);
        CHECK(a.id == b.id && a.status == b.status && a.filled == b.filled && a.resting == b.resting);
        if (a.resting) ids.push_back(a.id);
        CHECK(fast.trades() == ref.trades());
      } else if (kind < 62) {
        auto a = fast.market(side, qty * 3);
        auto b = ref.market(side, qty * 3);
        CHECK(a.id == b.id && a.status == b.status && a.filled == b.filled);
        CHECK(fast.trades() == ref.trades());
      } else if (kind < 92 && !ids.empty()) {
        const std::size_t k = rng() % ids.size();
        CHECK(fast.cancel(ids[k]) == ref.cancel(ids[k]));
        ids[k] = ids.back();
        ids.pop_back();
      } else if (!ids.empty()) {
        const OrderId id = ids[rng() % ids.size()];
        const Qty nq = static_cast<Qty>(rng() % 60);
        CHECK(fast.amend(id, nq) == ref.amend(id, nq));
      }
      CHECK(fast.best_bid() == ref.best_bid());
      CHECK(fast.best_ask() == ref.best_ask());
      CHECK(fast.order_count() == ref.order_count());
      if (step % 100 == 0) {
        CHECK(fast.depth(Side::Buy, 20) == ref.depth(Side::Buy, 20));
        CHECK(fast.depth(Side::Sell, 20) == ref.depth(Side::Sell, 20));
      }
      if (g_failed) { std::printf("  diverged at seed %llu step %d\n", (unsigned long long)seed, step); return; }
    }
  }
}

int main() {
  for (auto& t : registry()) {
    const int before = g_failed;
    t.fn();
    std::printf("%s %s\n", g_failed == before ? "ok  " : "FAIL", t.name);
  }
  std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
