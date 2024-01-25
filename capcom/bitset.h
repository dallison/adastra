// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#pragma once

#include <strings.h>

#include <cstddef>
#include <cstdlib>
#include <vector>

namespace adastra::capcom {

class BitSet {
 public:
  // Allocate the first free bit.
  uint32_t Allocate();

  // Clear a bit.
  void Clear(uint32_t bit);

  // Set a bit.
  void Set(uint32_t bit);

  // Is the bitset empty (all bits clear)?
  bool IsEmpty() const;

  // Is the given bit set?
  bool Contains(uint32_t bit) const;

  void ClearAll();

  void Print() const {
    for (auto v : bits_) {
      for (int i = 0; i < 64; i++) {
        std::cout << ((v & (1LL << i)) ? "1" : "0");
      }
      std::cout << std::endl;
    }
  }

 private:
  // Note the use of explicit long long type here because
  // we use ffsll to look for the set bits and that is
  // explicit in its use of long long.
  std::vector<long long> bits_;
};

inline uint32_t BitSet::Allocate() {
  uint32_t start = 0;
  for (;;) {
    for (uint32_t i = start; i < bits_.size(); i++) {
      uint32_t bit = static_cast<uint32_t>(ffsll(~bits_[i]));
      if (bit != 0) {
        bits_[i] |= (1LL << (bit - 1));
        return i * 64 + (bit - 1);
      }
    }
    // Expand bit set and allocate again.  There's no point in
    // searching the whole bitset again because we know it won't
    // have any zero bits in it, so start at the newly added
    // word of zeroes.
    start = bits_.size();
    bits_.push_back(0);
  }
}

inline void BitSet::Clear(uint32_t bit) {
  uint32_t word = bit / 64;
  if (word < 0 || word > bits_.size()) {
    return;
  }
  // Add new word if it doesn't exist.
  if (word == bits_.size()) {
    bits_.push_back(0LL);
  }
  uint32_t b = bit % 64;
  bits_[word] &= ~(1LL << b);
}

inline void BitSet::Set(uint32_t bit) {
  uint32_t word = bit / 64;
  if (word < 0 || word > bits_.size()) {
    return;
  }
  // Add new word if it doesn't exist.
  if (word == bits_.size()) {
    bits_.push_back(0LL);
  }

  uint32_t b = bit % 64;
  bits_[word] |= (1LL << b);
}

inline bool BitSet::IsEmpty() const {
  for (uint32_t i = 0; i < bits_.size(); i++) {
    if (bits_[i] != 0) {
      return false;
    }
  }
  return true;
}

inline bool BitSet::Contains(uint32_t bit) const {
  uint32_t word = bit / 64;
  if (word < 0 || word >= bits_.size()) {
    return false;
  }
  uint32_t b = bit % 64;
  return (bits_[word] & (1LL << b)) != 0;
}

inline void BitSet::ClearAll() {
  for (size_t i = 0; i < bits_.size(); i++) {
    bits_[i] = 0LL;
  }
}
}  // namespace adastra::capcom
