// SPDX-License-Identifier: MIT

#ifndef JGLOCKFREE_SPSC_H_
#define JGLOCKFREE_SPSC_H_

#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>

#if defined(__x86_64__)
#include <immintrin.h>  // for _mm_pause
#endif

#include <jglockfree/config.h>

namespace jglockfree {

template <typename T, std::size_t NumSlots, typename Traits = DefaultTraits>
class SpscQueue {
 public:
  SpscQueue() : head_{0}, tail_{0} {}
  ~SpscQueue() noexcept = default;

  constexpr SpscQueue(const SpscQueue &) = delete;
  constexpr SpscQueue &operator=(const SpscQueue &) = delete;

  SpscQueue(SpscQueue &&) = delete;
  SpscQueue &operator=(SpscQueue &&) = delete;

  [[nodiscard]] auto TryEnqueue(T value) -> bool;
  [[nodiscard]] auto TryDequeue() -> std::optional<T>;
  auto Enqueue(T value) -> void;
  [[nodiscard]] auto Dequeue() -> T;
  [[nodiscard]] constexpr auto TryEnqueueUnsignalled(T value) -> bool;
  [[nodiscard]] constexpr auto TryDequeueUnsignalled() -> std::optional<T>;

 private:
  union Slot {
    constexpr Slot() noexcept: empty() {}
    ~Slot() noexcept {};

    T value;
    std::monostate empty;
  };

  alignas(Traits::kCacheLineSize) std::array<Slot, NumSlots + 1> slots_;
  alignas(Traits::kCacheLineSize) std::atomic<std::size_t> head_;
  alignas(Traits::kCacheLineSize) std::atomic<std::size_t> tail_;
  alignas(Traits::kCacheLineSize) std::condition_variable not_empty_;
  alignas(Traits::kCacheLineSize) std::condition_variable not_full_;
  alignas(Traits::kCacheLineSize) std::mutex mutex_;
};

template <typename T, std::size_t NumSlots, typename Traits>
constexpr bool SpscQueue<T, NumSlots, Traits>::TryEnqueueUnsignalled(T value) {
  const std::size_t head = head_.load(std::memory_order_acquire);
  const std::size_t tail = tail_.load(std::memory_order_relaxed);
  const std::size_t new_tail = (tail + 1) % (NumSlots + 1);

  if (new_tail == head) {
    // Queue is full, return false
    return false;
  } else {
    std::construct_at(&slots_[tail].value, std::move(value));
    tail_.store(new_tail, std::memory_order_release);
    return true;
  }
}

template <typename T, std::size_t NumSlots, typename Traits>
bool SpscQueue<T, NumSlots, Traits>::TryEnqueue(T value) {
  const bool success = TryEnqueueUnsignalled(std::move(value));
  if (success) {
    std::lock_guard<std::mutex> lock{mutex_};
    not_empty_.notify_one();
  }
  return success;
}

template <typename T, std::size_t NumSlots, typename Traits>
void SpscQueue<T, NumSlots, Traits>::Enqueue(T value) {
  // Fast path: spin for a bit
  for (int i = 0; i < Traits::kSpinCount; ++i) {
    // We can't use TryEnqueueUnsignalled because it consumes the value,
    // so inline the logic
    const std::size_t head = head_.load(std::memory_order_acquire);
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    const std::size_t new_tail = (tail + 1) % (NumSlots + 1);

    if (new_tail != head) {
      // Space available, now we can move
      std::construct_at(&slots_[tail].value, std::move(value));
      tail_.store(new_tail, std::memory_order_release);
      not_empty_.notify_one();
      return;
    }
#if defined(__x86_64__)
    _mm_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#else
    std::this_thread::yield();
#endif
  }

  // Slow path: give up and block
  std::unique_lock<std::mutex> lock{mutex_};
  not_full_.wait(lock, [this] {
    const std::size_t head = head_.load(std::memory_order_acquire);
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    return (tail + 1) % (NumSlots + 1) != head;
  });

  // Now we have space, move the value in
  const std::size_t tail = tail_.load(std::memory_order_relaxed);
  const std::size_t new_tail = (tail + 1) % (NumSlots + 1);

  // Move the value into the slot
  std::construct_at(&slots_[tail].value, std::move(value));

  tail_.store(new_tail, std::memory_order_release);
  not_empty_.notify_one();
}

template <typename T, std::size_t NumSlots, typename Traits>
constexpr std::optional<T>
SpscQueue<T, NumSlots, Traits>::TryDequeueUnsignalled() {
  const std::size_t head = head_.load(std::memory_order_relaxed);
  const std::size_t tail = tail_.load(std::memory_order_acquire);

  if (head == tail) {
    // Queue is empty, return false
    return std::nullopt;
  } else {
    const std::size_t new_head = (head + 1) % (NumSlots + 1);

    // Move the result out of the slot and destroy the slot in place
    T result = std::move(slots_[head].value);
    std::destroy_at(&slots_[head].value);

    head_.store(new_head, std::memory_order_release);
    return result;
  }
}

template <typename T, std::size_t NumSlots, typename Traits>
std::optional<T> SpscQueue<T, NumSlots, Traits>::TryDequeue() {
  std::optional<T> result = TryDequeueUnsignalled();
  if (result.has_value()) {
    std::lock_guard<std::mutex> lock{mutex_};
    not_full_.notify_one();
  }
  return result;
}

template <typename T, std::size_t NumSlots, typename Traits>
T SpscQueue<T, NumSlots, Traits>::Dequeue() {
  // Fast path: spin for a bit
  for (std::size_t i = 0; i < Traits::kSpinCount; ++i) {
    std::optional<T> result = TryDequeueUnsignalled();
    if (result.has_value()) {
      not_full_.notify_one();
      return std::move(*result);
    }
#if defined(__x86_64__)
    _mm_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#endif
  }

  // Slow path: give up and block
  std::optional<T> result;
  std::unique_lock<std::mutex> lock{mutex_};
  not_empty_.wait(lock, [this, &result] {
    result = TryDequeueUnsignalled();
    return result.has_value();
  });
  not_full_.notify_one();
  return std::move(*result);
}

}  // namespace jglockfree

#endif  // JGLOCKFREE_SPSC_H_
