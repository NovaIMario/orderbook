// Hierarchical bitset: layer 0 holds one bit per index; bit j of layer k+1 is set when word j of
// layer k is non-zero. next(i) and prev(i) find the nearest set index in O(log64 N) word scans.
#pragma once

#include <bit>
#include <cstdint>
#include <vector>

namespace ob {

class HierBitset {
 public:
  static constexpr std::int64_t npos = -1;

  explicit HierBitset(std::size_t n) : n_(n) {
    std::size_t words = (n + 63) / 64;
    do {
      layers_.emplace_back(words, 0);
      words = (words + 63) / 64;
    } while (layers_.back().size() > 1);
  }

  bool test(std::size_t i) const { return (layers_[0][i >> 6] >> (i & 63)) & 1; }

  void set(std::size_t i) {
    for (auto& layer : layers_) {
      const bool was_empty = layer[i >> 6] == 0;
      layer[i >> 6] |= std::uint64_t{1} << (i & 63);
      if (!was_empty) return;
      i >>= 6;
    }
  }

  void clear(std::size_t i) {
    for (auto& layer : layers_) {
      layer[i >> 6] &= ~(std::uint64_t{1} << (i & 63));
      if (layer[i >> 6] != 0) return;
      i >>= 6;
    }
  }

  // Smallest set index >= i, or npos.
  std::int64_t next(std::int64_t i) const {
    if (i < 0) i = 0;
    if (static_cast<std::size_t>(i) >= n_) return npos;
    std::size_t layer = 0, pos = static_cast<std::size_t>(i);
    for (;;) {
      if (layer == layers_.size()) return npos;
      const std::size_t w = pos >> 6;
      if (w >= layers_[layer].size()) return npos;
      const std::uint64_t bits = layers_[layer][w] & (~std::uint64_t{0} << (pos & 63));
      if (bits) { pos = (w << 6) | std::countr_zero(bits); break; }
      pos = w + 1;
      ++layer;
    }
    while (layer > 0) { --layer; pos = (pos << 6) | std::countr_zero(layers_[layer][pos]); }
    return static_cast<std::int64_t>(pos);
  }

  // Largest set index <= i, or npos.
  std::int64_t prev(std::int64_t i) const {
    if (i < 0) return npos;
    if (static_cast<std::size_t>(i) >= n_) i = static_cast<std::int64_t>(n_) - 1;
    std::size_t layer = 0, pos = static_cast<std::size_t>(i);
    for (;;) {
      if (layer == layers_.size()) return npos;
      const std::size_t w = pos >> 6;
      const unsigned b = pos & 63;
      const std::uint64_t mask = b == 63 ? ~std::uint64_t{0} : (std::uint64_t{1} << (b + 1)) - 1;
      const std::uint64_t bits = layers_[layer][w] & mask;
      if (bits) { pos = (w << 6) | (63 - std::countl_zero(bits)); break; }
      if (w == 0) return npos;
      pos = w - 1;
      ++layer;
    }
    while (layer > 0) { --layer; pos = (pos << 6) | (63 - std::countl_zero(layers_[layer][pos])); }
    return static_cast<std::int64_t>(pos);
  }

 private:
  std::size_t n_;
  std::vector<std::vector<std::uint64_t>> layers_;
};

}  // namespace ob
