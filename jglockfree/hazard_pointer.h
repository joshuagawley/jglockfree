// SPDX-License-Identifier: MIT

#ifndef JGLOCKFREE_HAZARD_POINTER_H_
#define JGLOCKFREE_HAZARD_POINTER_H_

#include <jglockfree/config.h>

#include <algorithm>
#include <mutex>
#include <stdexcept>

namespace jglockfree {

struct RetiredNode {
  void *ptr;
  void (*deleter)(void *);
};

template <typename Traits = DefaultTraits,
          std::size_t NumSlots = Traits::kDefaultHazardSlots>
class HazardPointer {
 public:
  HazardPointer();
  ~HazardPointer() noexcept;

  template <typename T>
  [[nodiscard]] constexpr auto Protect(std::atomic<T *> &source) noexcept
      -> T *;

  template <typename T>
  [[nodiscard]] static constexpr auto IsProtected(T *ptr) noexcept -> bool;

  constexpr auto Clear() noexcept -> void;

  template <typename T, typename Deleter>
  constexpr static auto Retire(T *ptr, Deleter deleter) -> void;

 private:
  constexpr static auto Scan() -> void;

  struct HazardPointerSlot {
    alignas(Traits::kCacheLineSize) std::atomic<void *> ptr;
  };

  static inline std::array<HazardPointerSlot, NumSlots> slots_{};
  static inline std::atomic<std::size_t> next_slot_{0};

  static inline std::mutex free_list_guard_{};
  static inline std::vector<std::size_t> free_list_{};

  thread_local static inline std::vector<RetiredNode> retired_;
  static constexpr std::size_t kRetireThreshold = 2 * NumSlots;

  alignas(Traits::kCacheLineSize) std::atomic<void *> *slot_{nullptr};
  std::size_t slot_index_{};
};

template <typename Traits, std::size_t NumSlots>
HazardPointer<Traits, NumSlots>::HazardPointer() {
  {
    std::lock_guard lock{free_list_guard_};
    if (not free_list_.empty()) {
      slot_index_ = free_list_.back();
      free_list_.pop_back();
    } else {
      slot_index_ = next_slot_.fetch_add(1, std::memory_order_relaxed);
      if (slot_index_ >= NumSlots) {
        throw std::runtime_error("Hazard pointer slots exhausted");
      }
    }
  }
  slot_ = &slots_[slot_index_].ptr;
}

template <typename Traits, std::size_t NumSlots>
HazardPointer<Traits, NumSlots>::~HazardPointer() noexcept {
  Clear();
  std::lock_guard lock{free_list_guard_};
  free_list_.push_back(slot_index_);
}

template <typename Traits, std::size_t NumSlots>
template <typename T>
constexpr T *HazardPointer<Traits, NumSlots>::Protect(
    std::atomic<T *> &source) noexcept {
  while (true) {
    T *ptr = source.load(std::memory_order_acquire);
    slot_->store(ptr, std::memory_order_seq_cst);
    T *current = source.load(std::memory_order_seq_cst);
    if (ptr == current) {
      return ptr;
    }
  }
}

template <typename Traits, std::size_t NumSlots>
constexpr void HazardPointer<Traits, NumSlots>::Clear() noexcept {
  slot_->store(nullptr, std::memory_order_release);
}

template <typename Traits, std::size_t NumSlots>
template <typename T>
constexpr bool HazardPointer<Traits, NumSlots>::IsProtected(T *ptr) noexcept {
  const std::size_t count = next_slot_.load(std::memory_order_acquire);
  for (std::size_t i = 0; i < count; ++i) {
    if (slots_[i].ptr.load(std::memory_order_acquire) == ptr) {
      return true;
    }
  }

  return false;
}

template <typename Traits, std::size_t NumSlots>
template <typename T, typename Deleter>
constexpr void HazardPointer<Traits, NumSlots>::Retire(T *ptr,
                                                       Deleter deleter) {
  retired_.emplace_back(ptr, deleter);
  if (retired_.size() >= kRetireThreshold) {
    Scan();
  }
}

template <typename Traits, std::size_t NumSlots>
constexpr void HazardPointer<Traits, NumSlots>::Scan() {
  const std::size_t count = next_slot_.load(std::memory_order_acquire);

  std::array<void *, NumSlots> protected_ptrs{};
  for (std::size_t i = 0; i < count; ++i) {
    protected_ptrs[i] = slots_[i].ptr.load(std::memory_order_acquire);
  }

  // for large N (> 256), use sort + binary search
  // for small N (<= 256), use linear search
  if constexpr (NumSlots > 256) {
    std::sort(std::begin(protected_ptrs),
              std::next(std::begin(protected_ptrs), count));
  }

  std::erase_if(retired_, [&protected_ptrs, count](const RetiredNode &node) {
    // return false -> keep in retired list (still protected)
    // return true -> remove from retired list (safe to free)
    if constexpr (NumSlots <= 256) {
      for (std::size_t i = 0; i < count; ++i) {
        if (protected_ptrs[i] == node.ptr) {
          return false;
        }
      }
      node.deleter(node.ptr);
      return true;
    } else {
      if (std::binary_search(std::begin(protected_ptrs),
                             std::next(std::begin(protected_ptrs), count),
                             node.ptr)) {
        return false;
      }
      node.deleter(node.ptr);
      return true;
    }
  });
}

template <std::size_t NumSlots>
using HazardPointerN = HazardPointer<DefaultTraits, NumSlots>;

}  // namespace jglockfree

#endif  // JGLOCKFREE_HAZARD_POINTER_H_
