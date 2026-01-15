// SPDX-License-Identifier: MIT

#ifndef JGLOCKFREE_HAZARD_POINTER_H_
#define JGLOCKFREE_HAZARD_POINTER_H_

#include <algorithm>
#include <mutex>

#include <jglockfree/config.h>

namespace jglockfree {

/**
 * @brief
 * @details
 */
struct RetiredNode {
  void *ptr;              ///< Pointer to the retired object.
  void (*deleter)(void *);  ///< Deleter function to reclaim the object.
};

/**
 * @brief
 * @details
 *
 * @tparam Traits The traits class providing configuration constants such as
 *         cache line size and default slot count.
 * @tparam NumSlots The maximum number of hazard pointer slots available.
 *         Defaults to `Traits::kDefaultHazardSlots`.
 *
 * @see HazardPointerN For a convenience alias when using DefaultTraits.
 */
template <typename Traits = DefaultTraits, std::size_t NumSlots = Traits::kDefaultHazardSlots>
class HazardPointer {
 public:
  /**
   * @brief
   * @details
   *
   * @throws std::runtime_error if no hazard pointer slots are available.
   */
  HazardPointer();

  ~HazardPointer() noexcept;

  /**
   * @brief
   * @details
   *
   * @tparam T The type of the pointer to protect.
   * @param source The atomic pointer to protect.
   * @return The protected pointer value.
   */
  template <typename T>
  [[nodiscard]] constexpr auto Protect(std::atomic<T *> &source) noexcept
      -> T *;

  /**
   * @brief
   * @details
   *
   * @tparam T The type of the pointer to check.
   * @param ptr The pointer to check for protection.
   * @return true if the pointer is currently protected by any hazard pointer,
   *         false otherwise.
   */
  template <typename T>
  [[nodiscard]] static constexpr auto IsProtected(T *ptr) noexcept -> bool;

  /**
   * @brief
   * @details
   */
  constexpr auto Clear() noexcept -> void;

  /**
   * @brief
   * @details
   *
   * @tparam T The type of the pointer to retire.
   * @tparam Deleter The type of the deleter function.
   * @param ptr The pointer to retire.
   * @param deleter The deleter function to reclaim the object.
   */
  template <typename T, typename Deleter>
  constexpr static auto Retire(T *ptr, Deleter deleter) -> void;

 private:
  constexpr static auto Scan() -> void;

  struct HazardPointerSlot {
    alignas(Traits::kCacheLineSize) std::atomic<void *> ptr;
  };
  static_assert(sizeof(HazardPointerSlot) == std::hardware_destructive_interference_size,
              "HazardSlot is not padded to cache line size!");

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
    const auto ptr = source.load(std::memory_order_acquire);
    slot_->store(ptr, std::memory_order_seq_cst);
    const auto current = source.load(std::memory_order_seq_cst);
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
  const auto count = next_slot_.load(std::memory_order_acquire);
  for (auto i = std::size_t{0}; i < count; ++i) {
    if (slots_[i].ptr.load(std::memory_order_acquire) == ptr) {
      return true;
    }
  }

  return false;
}

template <typename Traits, std::size_t NumSlots>
template <typename T, typename Deleter>
constexpr void HazardPointer<Traits, NumSlots>::Retire(T *ptr, Deleter deleter) {
  retired_.emplace_back(ptr, deleter);
  if (retired_.size() >= kRetireThreshold) {
    Scan();
  }
}

template <typename Traits, std::size_t NumSlots>
constexpr void HazardPointer<Traits, NumSlots>::Scan() {
  const auto count = next_slot_.load(std::memory_order_acquire);

  std::array<void *, NumSlots> protected_ptrs{};
  for (std::size_t i = 0; i < count; ++i) {
    protected_ptrs[i] = slots_[i].ptr.load(std::memory_order_acquire);
  }

  // for large N (> 256), use sort + binary search
  // for small N (<= 256), use linear search
  if constexpr (NumSlots > 256) {
    std::ranges::sort(protected_ptrs);
  }

  std::erase_if(retired_, [&protected_ptrs, count](const RetiredNode &node) {
    if constexpr (NumSlots <= 256) {
      for (std::size_t i = 0; i < count; ++i) {
        if (protected_ptrs[i] == node.ptr) {
          return false;
        }
      }
      node.deleter(node.ptr);
      return true;
    } else {
      if (std::binary_search(protected_ptrs.begin(), protected_ptrs.end(), node.ptr)) {
        return false;
      }
      node.deleter(node.ptr);
      return true;
    }
  });
}

/**
 * @brief Convenience alias for HazardPointer with a custom slot count.
 * @details Provides a simpler syntax for specifying the number of hazard pointer
 *          slots while using DefaultTraits. Equivalent to
 *          `HazardPointer<DefaultTraits, NumSlots>`.
 *
 * @tparam NumSlots The maximum number of hazard pointer slots available.
 *
 * @see HazardPointer
 */
template <std::size_t NumSlots>
using HazardPointerN = HazardPointer<DefaultTraits, NumSlots>;

}  // namespace jglockfree

#endif  // JGLOCKFREE_HAZARD_POINTER_H_
