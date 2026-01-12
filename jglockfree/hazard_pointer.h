// SPDX-License-Identifier: MIT

#ifndef JGLOCKFREE_HAZARD_POINTER_H_
#define JGLOCKFREE_HAZARD_POINTER_H_

#include <mutex>
#include <ranges>
#include <unordered_set>

namespace jglockfree {

struct RetiredNode {
  void *ptr;
  void (*deleter)(void *);
};

template <std::size_t NumSlots = 128>
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

  template <typename T>
  constexpr static auto Retire(T *ptr) -> void;

 private:
  constexpr static auto Scan() -> void;

  static inline std::array<std::atomic<void *>, NumSlots> slots_{};
  static inline std::atomic<std::size_t> next_slot_{0};

  static inline std::mutex free_list_guard_{};
  static inline std::vector<std::size_t> free_list_{};

  thread_local static inline std::vector<RetiredNode> retired_;
  static constexpr std::size_t kRetireThreshold = 2 * NumSlots;

  std::atomic<void *> *slot_{nullptr};
  std::size_t slot_index_{};
};

template <std::size_t NumSlots>
HazardPointer<NumSlots>::HazardPointer() {
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
  slot_ = &slots_[slot_index_];
}

template <std::size_t NumSlots>
HazardPointer<NumSlots>::~HazardPointer() noexcept {
  Clear();
  std::lock_guard lock{free_list_guard_};
  free_list_.push_back(slot_index_);
}

template <std::size_t NumSlots>
template <typename T>
constexpr T *HazardPointer<NumSlots>::Protect(
    std::atomic<T *> &source) noexcept {
  while (true) {
    const auto ptr = source.load(std::memory_order_acquire);
    slot_->store(ptr, std::memory_order_seq_cst);
    const auto current = source.load(std::memory_order_seq_cst);
    if (ptr == current) {
      return ptr;
    }
  }
}

template <std::size_t NumSlots>
constexpr void HazardPointer<NumSlots>::Clear() noexcept {
  slot_->store(nullptr, std::memory_order_release);
}

template <std::size_t NumSlots>
template <typename T>
constexpr bool HazardPointer<NumSlots>::IsProtected(T *ptr) noexcept {
  const auto count = next_slot_.load(std::memory_order_acquire);
  for (auto i = std::size_t{0}; i < count; ++i) {
    if (slots_[i].load(std::memory_order_acquire) == ptr) {
      return true;
    }
  }

  return false;
}

template <std::size_t NumSlots>
template <typename T>
constexpr void HazardPointer<NumSlots>::Retire(T *ptr) {
  retired_.emplace_back(ptr, [](void *p) { delete static_cast<T *>(p); });
  if (retired_.size() >= kRetireThreshold) {
    Scan();
  }
}

template <std::size_t NumSlots>
constexpr void HazardPointer<NumSlots>::Scan() {
  const auto count = next_slot_.load(std::memory_order_acquire);
  const auto loads = std::views::iota(std::size_t{0}, count) |
                     std::views::transform([](auto i) {
                       return slots_[i].load(std::memory_order_acquire);
                     });
  const std::unordered_set<void *> protected_ptrs(loads.begin(), loads.end());

  std::erase_if(retired_, [&protected_ptrs](const RetiredNode &node) {
    if (protected_ptrs.contains(node.ptr)) {
      return false;
    }
    node.deleter(node.ptr);
    return true;
  });
}

}  // namespace jglockfree

#endif  // JGLOCKFREE_HAZARD_POINTER_H_
