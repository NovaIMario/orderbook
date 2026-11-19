// A price-time priority limit order book for a single instrument.
//
// Design (all hot-path operations are O(1) or O(log64 N) with no heap allocation once warm):
//   * Prices are integer ticks in [1, max_ticks). Each side has a flat array of price levels
//     indexed by tick, so finding a level is a single array access.
//   * A hierarchical bitset per side marks non-empty levels. Finding the next best price after a
//     level empties takes at most log64(max_ticks) word scans (3 for the default 262,144 ticks).
//   * Orders live in a pooled vector and form an intrusive doubly linked FIFO list per level, so
//     cancelling any order is O(1).
//   * Order ids are assigned by the book in sequence, so the id -> slot lookup is a vector index.
#pragma once

#include <cstdint>
#include <limits>
#include <vector>

#include "ob/bitset.hpp"

namespace ob {

using Price = std::int64_t;
using Qty = std::int64_t;
using OrderId = std::uint64_t;

enum class Side : std::uint8_t { Buy, Sell };
enum class TimeInForce : std::uint8_t {
  GTC,  // good till cancelled: any unfilled part rests on the book
  IOC,  // immediate or cancel: fill what is possible now, drop the rest
  FOK,  // fill or kill: fill completely now or do nothing
};
enum class Status : std::uint8_t { Rejected, Filled, PartiallyFilled, Resting, Cancelled };

struct Trade {
  OrderId maker;
  OrderId taker;
  Price price;
  Qty qty;
  Side taker_side;
  bool operator==(const Trade&) const = default;
};

struct Result {
  OrderId id = 0;  // 0 when rejected
  Status status = Status::Rejected;
  Qty filled = 0;
  Qty resting = 0;
};

struct Level {
  Price price;
  Qty qty;
  std::uint32_t orders;
  bool operator==(const Level&) const = default;
};

class OrderBook {
 public:
  static constexpr Price kDefaultTicks = 1 << 18;

  explicit OrderBook(Price max_ticks = kDefaultTicks);

  // Submit a limit order. Crosses the spread first, then rests (GTC) or is dropped (IOC/FOK).
  Result limit(Side side, Price price, Qty qty, TimeInForce tif = TimeInForce::GTC);
  // Submit a market order: takes liquidity at any price; any unfilled part is cancelled.
  Result market(Side side, Qty qty);
  // Cancel a resting order. Returns false if it is unknown or already gone.
  bool cancel(OrderId id);
  // Change a resting order's size. Reducing keeps time priority; increasing loses it (the order
  // moves to the back of its level). A new quantity of 0 cancels.
  bool amend(OrderId id, Qty new_qty);

  // Trades produced by the most recent limit/market call, in execution order.
  const std::vector<Trade>& trades() const { return trades_; }

  bool has_bid() const { return best_bid_ >= 0; }
  bool has_ask() const { return best_ask_ < max_ticks_; }
  Price best_bid() const { return best_bid_; }
  Price best_ask() const { return best_ask_; }
  Qty volume_at(Side side, Price price) const;
  std::vector<Level> depth(Side side, std::size_t levels) const;
  std::size_t order_count() const { return live_orders_; }
  bool contains(OrderId id) const;

 private:
  static constexpr std::uint32_t kNil = std::numeric_limits<std::uint32_t>::max();

  struct Order {
    OrderId id;
    Price price;
    Qty qty;
    std::uint32_t prev;
    std::uint32_t next;
    Side side;
  };
  struct LevelSlot {
    std::uint32_t head = kNil;
    std::uint32_t tail = kNil;
    Qty total = 0;
    std::uint32_t count = 0;
  };

  template <Side S>
  Qty match(OrderId taker, Price limit, Qty qty);
  template <Side S>
  Qty available(Price limit, Qty want) const;
  void rest(OrderId id, Side side, Price price, Qty qty);
  void unlink(std::uint32_t slot);
  std::uint32_t alloc();
  OrderId next_id();

  Price max_ticks_;
  Price best_bid_;  // -1 when there are no bids
  Price best_ask_;  // max_ticks_ when there are no asks
  std::vector<LevelSlot> bids_, asks_;
  HierBitset bid_bits_, ask_bits_;
  std::vector<Order> pool_;
  std::vector<std::uint32_t> free_;
  std::vector<std::uint32_t> slot_of_;  // indexed by OrderId
  std::vector<Trade> trades_;
  std::size_t live_orders_ = 0;
};

}  // namespace ob
