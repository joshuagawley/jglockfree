// SPDX-License-Identifier: MIT

#ifndef JGLOCKFREE_CONFIG_H_
#define JGLOCKFREE_CONFIG_H_

#include <new>

namespace jglockfree {

struct DefaultTraits {
#ifdef __cpp_lib_hardware_interference_size
  static constexpr std::size_t kCacheLineSize =
      std::hardware_destructive_interference_size;
#else
  static constexpr std::size_t kCacheLineSize = 64;
#endif

  static constexpr std::size_t kSpinCount = 1000;
  static constexpr std::size_t kDefaultHazardSlots = 128;
};

}  // namespace jglockfree

#endif  // JGLOCKFREE_CONFIG_H_
