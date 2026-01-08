// SPDX-License-Identifier: MIT

#ifndef JGLOCKFREE_HAZARD_POINTER_H_
#define JGLOCKFREE_HAZARD_POINTER_H_

#include <algorithm>
#include <ranges>
#include <unordered_set>

namespace jglockfree {

inline constexpr std::size_t kMaxHazardPointers = 128;
inline constinit std::array<std::atomic<void *>, kMaxHazardPointers> kSlots{};
inline constinit std::atomic<std::size_t> kNextSlot{0};

struct RetiredNode {
  void *ptr;
  void (*deleter)(void *);
};

class HazardPointer {
public:
   HazardPointer() {
   const auto index = kNextSlot.fetch_add(1, std::memory_order_relaxed);
     if (index >= kMaxHazardPointers) {
       throw std::runtime_error("Hazard pointer slots exhausted");

     }
   slot_ = &kSlots[index];

  }

  template <typename T>
  constexpr T *Protect(std::atomic<T *> &source) {
    while (true) {
      const auto ptr = source.load(std::memory_order_acquire);
      slot_->store(ptr, std::memory_order_seq_cst);
      const auto current = source.load(std::memory_order_seq_cst);
      if (ptr == current) {
        return ptr;
      }
    }
  }

  constexpr void Clear() const {
    slot_->store(nullptr, std::memory_order_release);
  }

  template <typename T>
  static constexpr bool IsProtected(T *ptr) {
     const auto count = kNextSlot.load(std::memory_order_acquire);
     return std::ranges::any_of(std::ranges::views::iota(std::size_t{0}, count), [ptr](auto i) {
       return kSlots[i].load(std::memory_order_acquire) == ptr;
     });
  }

  template <typename T>
  static void Retire(T *ptr) {
     retired_.emplace_back(ptr, [](void *p) { delete static_cast<T *>(p); });
    if (retired_.size() >= kRetireThreshold) {
      Scan();
    }
  }
private:
  static void Scan() {
    const auto count = kNextSlot.load(std::memory_order_acquire);
    const auto loads = std::ranges::views::iota(std::size_t{0}, count) | std::views::transform([](auto i) { return kSlots[i].load(std::memory_order_acquire); });
    const std::unordered_set<void *> protected_ptrs(loads.begin(), loads.end());

    std::erase_if(retired_, [&protected_ptrs](const RetiredNode &node) {
      if (protected_ptrs.contains(node.ptr)) {
        return false;
      }
      node.deleter(node.ptr);
      return true;
    });
  }

  std::atomic<void *> *slot_{nullptr};

  static constexpr std::size_t kRetireThreshold = 2 * kMaxHazardPointers;
  thread_local static inline std::vector<RetiredNode> retired_;
};

} // namespace jglockfree

#endif // JGLOCKFREE_HAZARD_POINTER_H_
