#include "ob/order_book.hpp"

#include <algorithm>

namespace ob {

OrderBook::OrderBook(Price max_ticks)
    : max_ticks_(max_ticks),
      best_bid_(-1),
      best_ask_(max_ticks),
      bids_(static_cast<std::size_t>(max_ticks)),
      asks_(static_cast<std::size_t>(max_ticks)),
      bid_bits_(static_cast<std::size_t>(max_ticks)),
      ask_bits_(static_cast<std::size_t>(max_ticks)) {
  slot_of_.push_back(kNil);  // id 0 is never used
  trades_.reserve(64);
}

OrderId OrderBook::next_id() {
  slot_of_.push_back(kNil);
  return slot_of_.size() - 1;
}

std::uint32_t OrderBook::alloc() {
  if (!free_.empty()) {
    const std::uint32_t s = free_.back();
    free_.pop_back();
    return s;
  }
  pool_.emplace_back();
  return static_cast<std::uint32_t>(pool_.size() - 1);
}

// How much the opposite side could fill for a taker on side S up to `limit`, stopping early once
// `want` is reached. Used by fill-or-kill orders.
template <Side S>
Qty OrderBook::available(Price limit, Qty want) const {
  Qty got = 0;
  if constexpr (S == Side::Buy) {
    for (std::int64_t p = has_ask() ? best_ask_ : HierBitset::npos; p != HierBitset::npos && p <= limit;
         p = ask_bits_.next(p + 1)) {
      got += asks_[p].total;
      if (got >= want) break;
    }
  } else {
    for (std::int64_t p = has_bid() ? best_bid_ : HierBitset::npos; p != HierBitset::npos && p >= limit;
         p = bid_bits_.prev(p - 1)) {
      got += bids_[p].total;
      if (got >= want) break;
    }
  }
  return got;
}

// Fill a taker on side S against the opposite side, best price first and oldest order first
// within a price. Returns the quantity left unfilled.
template <Side S>
Qty OrderBook::match(OrderId taker, Price limit, Qty qty) {
  constexpr bool buy = S == Side::Buy;
  auto& levels = buy ? asks_ : bids_;
  auto& bits = buy ? ask_bits_ : bid_bits_;
  Price& best = buy ? best_ask_ : best_bid_;
  auto crosses = [&] { return buy ? (best < max_ticks_ && best <= limit) : (best >= 0 && best >= limit); };

  while (qty > 0 && crosses()) {
    LevelSlot& lvl = levels[best];
    while (qty > 0 && lvl.head != kNil) {
      const std::uint32_t s = lvl.head;
      Order& maker = pool_[s];
      const Qty fill = std::min(qty, maker.qty);
      trades_.push_back({maker.id, taker, best, fill, S});
      qty -= fill;
      maker.qty -= fill;
      lvl.total -= fill;
      if (maker.qty == 0) {
        lvl.head = maker.next;
        if (lvl.head != kNil) pool_[lvl.head].prev = kNil; else lvl.tail = kNil;
        --lvl.count;
        slot_of_[maker.id] = kNil;
        free_.push_back(s);
        --live_orders_;
      }
    }
    if (lvl.head == kNil) {
      bits.clear(best);
      if constexpr (buy) {
        const auto n = bits.next(best + 1);
        best = n == HierBitset::npos ? max_ticks_ : n;
      } else {
        const auto p = bits.prev(best - 1);
        best = p == HierBitset::npos ? -1 : p;
      }
    }
  }
  return qty;
}

void OrderBook::rest(OrderId id, Side side, Price price, Qty qty) {
  const std::uint32_t s = alloc();
  LevelSlot& lvl = side == Side::Buy ? bids_[price] : asks_[price];
  pool_[s] = Order{id, price, qty, lvl.tail, kNil, side};
  if (lvl.tail != kNil) pool_[lvl.tail].next = s; else lvl.head = s;
  lvl.tail = s;
  lvl.total += qty;
  ++lvl.count;
  slot_of_[id] = s;
  ++live_orders_;
  if (side == Side::Buy) {
    if (lvl.count == 1) bid_bits_.set(price);
    best_bid_ = std::max(best_bid_, price);
  } else {
    if (lvl.count == 1) ask_bits_.set(price);
    best_ask_ = std::min(best_ask_, price);
  }
}

Result OrderBook::limit(Side side, Price price, Qty qty, TimeInForce tif) {
  trades_.clear();
  if (qty <= 0 || price <= 0 || price >= max_ticks_) return {};
  if (tif == TimeInForce::FOK) {
    const Qty can = side == Side::Buy ? available<Side::Buy>(price, qty) : available<Side::Sell>(price, qty);
    if (can < qty) return {next_id(), Status::Cancelled, 0, 0};
  }
  const OrderId id = next_id();
  const Qty left = side == Side::Buy ? match<Side::Buy>(id, price, qty) : match<Side::Sell>(id, price, qty);
  const Qty filled = qty - left;
  if (left == 0) return {id, Status::Filled, filled, 0};
  if (tif != TimeInForce::GTC) return {id, filled ? Status::PartiallyFilled : Status::Cancelled, filled, 0};
  rest(id, side, price, left);
  return {id, filled ? Status::PartiallyFilled : Status::Resting, filled, left};
}

Result OrderBook::market(Side side, Qty qty) {
  trades_.clear();
  if (qty <= 0) return {};
  const OrderId id = next_id();
  const Qty left = side == Side::Buy ? match<Side::Buy>(id, max_ticks_ - 1, qty) : match<Side::Sell>(id, 1, qty);
  const Qty filled = qty - left;
  return {id, left == 0 ? Status::Filled : filled ? Status::PartiallyFilled : Status::Cancelled, filled, 0};
}

void OrderBook::unlink(std::uint32_t s) {
  Order& o = pool_[s];
  const bool buy = o.side == Side::Buy;
  LevelSlot& lvl = buy ? bids_[o.price] : asks_[o.price];
  if (o.prev != kNil) pool_[o.prev].next = o.next; else lvl.head = o.next;
  if (o.next != kNil) pool_[o.next].prev = o.prev; else lvl.tail = o.prev;
  lvl.total -= o.qty;
  --lvl.count;
  if (lvl.count == 0) {
    if (buy) {
      bid_bits_.clear(o.price);
      if (best_bid_ == o.price) {
        const auto p = bid_bits_.prev(o.price - 1);
        best_bid_ = p == HierBitset::npos ? -1 : p;
      }
    } else {
      ask_bits_.clear(o.price);
      if (best_ask_ == o.price) {
        const auto n = ask_bits_.next(o.price + 1);
        best_ask_ = n == HierBitset::npos ? max_ticks_ : n;
      }
    }
  }
}

bool OrderBook::contains(OrderId id) const { return id < slot_of_.size() && slot_of_[id] != kNil; }

bool OrderBook::cancel(OrderId id) {
  if (!contains(id)) return false;
  const std::uint32_t s = slot_of_[id];
  unlink(s);
  slot_of_[id] = kNil;
  free_.push_back(s);
  --live_orders_;
  return true;
}

bool OrderBook::amend(OrderId id, Qty new_qty) {
  if (!contains(id)) return false;
  if (new_qty <= 0) return cancel(id);
  const std::uint32_t s = slot_of_[id];
  Order& o = pool_[s];
  if (new_qty <= o.qty) {
    LevelSlot& lvl = o.side == Side::Buy ? bids_[o.price] : asks_[o.price];
    lvl.total -= o.qty - new_qty;
    o.qty = new_qty;
    return true;
  }
  // Increasing size: re-queue at the back of the same level, keeping the same id.
  const Side side = o.side;
  const Price price = o.price;
  unlink(s);
  free_.push_back(s);
  --live_orders_;
  rest(id, side, price, new_qty);
  return true;
}

Qty OrderBook::volume_at(Side side, Price price) const {
  if (price <= 0 || price >= max_ticks_) return 0;
  return side == Side::Buy ? bids_[price].total : asks_[price].total;
}

std::vector<Level> OrderBook::depth(Side side, std::size_t levels) const {
  std::vector<Level> out;
  if (side == Side::Buy) {
    for (std::int64_t p = has_bid() ? best_bid_ : HierBitset::npos; p != HierBitset::npos && out.size() < levels;
         p = bid_bits_.prev(p - 1))
      out.push_back({p, bids_[p].total, bids_[p].count});
  } else {
    for (std::int64_t p = has_ask() ? best_ask_ : HierBitset::npos; p != HierBitset::npos && out.size() < levels;
         p = ask_bits_.next(p + 1))
      out.push_back({p, asks_[p].total, asks_[p].count});
  }
  return out;
}

template Qty OrderBook::match<Side::Buy>(OrderId, Price, Qty);
template Qty OrderBook::match<Side::Sell>(OrderId, Price, Qty);

}  // namespace ob
