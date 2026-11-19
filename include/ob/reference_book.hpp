// The simplest correct order book I could write: std::map of price -> std::list of orders, and a
// hash map from id to iterator. It exists to check OrderBook in randomised tests and to give
// the benchmark a baseline. It mirrors OrderBook's public interface and id assignment.
#pragma once

#include <algorithm>
#include <functional>
#include <list>
#include <map>
#include <unordered_map>
#include <vector>

#include "ob/order_book.hpp"

namespace ob {

class ReferenceBook {
 public:
  explicit ReferenceBook(Price max_ticks = OrderBook::kDefaultTicks) : max_ticks_(max_ticks) {}

  Result limit(Side side, Price price, Qty qty, TimeInForce tif = TimeInForce::GTC) {
    trades_.clear();
    if (qty <= 0 || price <= 0 || price >= max_ticks_) return {};
    if (tif == TimeInForce::FOK && available(side, price) < qty) return {++last_id_, Status::Cancelled, 0, 0};
    const OrderId id = ++last_id_;
    const Qty left = match(side, id, price, qty);
    const Qty filled = qty - left;
    if (left == 0) return {id, Status::Filled, filled, 0};
    if (tif != TimeInForce::GTC) return {id, filled ? Status::PartiallyFilled : Status::Cancelled, filled, 0};
    auto& q = side == Side::Buy ? bids_[price] : asks_[price];
    q.push_back({id, left});
    where_[id] = {side, price, std::prev(q.end())};
    return {id, filled ? Status::PartiallyFilled : Status::Resting, filled, left};
  }

  Result market(Side side, Qty qty) {
    trades_.clear();
    if (qty <= 0) return {};
    const OrderId id = ++last_id_;
    const Qty left = match(side, id, side == Side::Buy ? max_ticks_ - 1 : 1, qty);
    const Qty filled = qty - left;
    return {id, left == 0 ? Status::Filled : filled ? Status::PartiallyFilled : Status::Cancelled, filled, 0};
  }

  bool cancel(OrderId id) {
    auto it = where_.find(id);
    if (it == where_.end()) return false;
    auto [side, price, pos] = it->second;
    erase(side, price, pos);
    where_.erase(it);
    return true;
  }

  bool amend(OrderId id, Qty new_qty) {
    auto it = where_.find(id);
    if (it == where_.end()) return false;
    if (new_qty <= 0) return cancel(id);
    auto& [side, price, pos] = it->second;
    if (new_qty <= pos->qty) { pos->qty = new_qty; return true; }
    erase(side, price, pos);
    auto& q = side == Side::Buy ? bids_[price] : asks_[price];
    q.push_back({id, new_qty});
    pos = std::prev(q.end());
    return true;
  }

  const std::vector<Trade>& trades() const { return trades_; }
  bool has_bid() const { return !bids_.empty(); }
  bool has_ask() const { return !asks_.empty(); }
  Price best_bid() const { return bids_.empty() ? -1 : bids_.rbegin()->first; }
  Price best_ask() const { return asks_.empty() ? max_ticks_ : asks_.begin()->first; }
  std::size_t order_count() const { return where_.size(); }

  std::vector<Level> depth(Side side, std::size_t levels) const {
    std::vector<Level> out;
    auto add = [&](const auto& kv) {
      Qty total = 0;
      for (const auto& o : kv.second) total += o.qty;
      out.push_back({kv.first, total, static_cast<std::uint32_t>(kv.second.size())});
    };
    if (side == Side::Buy) {
      for (auto it = bids_.rbegin(); it != bids_.rend() && out.size() < levels; ++it) add(*it);
    } else {
      for (auto it = asks_.begin(); it != asks_.end() && out.size() < levels; ++it) add(*it);
    }
    return out;
  }

 private:
  struct Entry { OrderId id; Qty qty; };
  using Queue = std::list<Entry>;
  struct Where { Side side; Price price; Queue::iterator pos; };

  Qty available(Side side, Price limit) const {
    Qty got = 0;
    if (side == Side::Buy) {
      for (const auto& [p, q] : asks_) { if (p > limit) break; for (const auto& o : q) got += o.qty; }
    } else {
      for (auto it = bids_.rbegin(); it != bids_.rend() && it->first >= limit; ++it) for (const auto& o : it->second) got += o.qty;
    }
    return got;
  }

  Qty match(Side side, OrderId taker, Price limit, Qty qty) {
    while (qty > 0) {
      Queue* q;
      Price p;
      if (side == Side::Buy) {
        if (asks_.empty() || asks_.begin()->first > limit) break;
        p = asks_.begin()->first; q = &asks_.begin()->second;
      } else {
        if (bids_.empty() || bids_.rbegin()->first < limit) break;
        p = bids_.rbegin()->first; q = &bids_.rbegin()->second;
      }
      while (qty > 0 && !q->empty()) {
        Entry& m = q->front();
        const Qty fill = std::min(qty, m.qty);
        trades_.push_back({m.id, taker, p, fill, side});
        qty -= fill; m.qty -= fill;
        if (m.qty == 0) { where_.erase(m.id); q->pop_front(); }
      }
      if (q->empty()) (side == Side::Buy ? asks_ : bids_).erase(p);
    }
    return qty;
  }

  void erase(Side side, Price price, Queue::iterator pos) {
    auto& book = side == Side::Buy ? bids_ : asks_;
    auto lvl = book.find(price);
    lvl->second.erase(pos);
    if (lvl->second.empty()) book.erase(lvl);
  }

  Price max_ticks_;
  OrderId last_id_ = 0;
  std::map<Price, Queue> bids_, asks_;
  std::unordered_map<OrderId, Where> where_;
  std::vector<Trade> trades_;
};

}  // namespace ob
