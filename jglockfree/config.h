// SPDX-License-Identifier: MIT

#ifndef JGLOCKFREE_CONFIG_H_
#define JGLOCKFREE_CONFIG_H_

#include <cstddef>
#include <new>

namespace jglockfree {

struct DefaultTraits {
  // Use a fixed cache line size to avoid GCC's -Winterference-size warning.
  // std::hardware_destructive_interference_size can vary between compiler
  // versions and CPU tuning flags, making it unsuitable for ABI-stable code.
  static constexpr std::size_t kCacheLineSize = 64;

  static constexpr int kSpinCount = 1000;
  static constexpr int kDefaultHazardSlots = 128;
};

}  // namespace jglockfree

#endif  // JGLOCKFREE_CONFIG_H_
