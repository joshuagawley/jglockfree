// SPDX-License-Identifier: MIT

#ifndef JGLOCKFREE_SPSC_H
#define JGLOCKFREE_SPSC_H

#include <array>
#include <atomic>
#include <mutex>
#include <new>
#include <optional>
#include <ranges>

namespace jglockfree {

template <typename T, std::size_t NumSlots>
class SpscQueue {
 public:
  SpscQueue() : head_{0}, tail_{0} {}
  ~SpscQueue() noexcept = default;

  constexpr SpscQueue(const SpscQueue &) = delete;
  constexpr SpscQueue &operator=(const SpscQueue &) = delete;

  SpscQueue(SpscQueue &&) = delete;
  SpscQueue &operator=(SpscQueue &&) = delete;

  [[nodiscard]] constexpr auto TryEnqueue(T value) -> bool;
  [[nodiscard]] constexpr auto TryDequeue() -> std::optional<T>;
  constexpr auto Enqueue(T value) -> void;
  [[nodiscard]] constexpr auto Dequeue() -> T;
  [[nodiscard]] constexpr auto TryEnqueueUnsignalled(T value) -> bool;
  [[nodiscard]] constexpr auto TryDequeueUnsignalled() -> std::optional<T>;

 private:
  alignas(std::hardware_destructive_interference_size)
      std::array<T, NumSlots + 1> slots_;
  alignas(std::hardware_destructive_interference_size)
      std::atomic<std::size_t> head_;
  alignas(std::hardware_destructive_interference_size)
      std::atomic<std::size_t> tail_;
  alignas(std::hardware_destructive_interference_size) std::condition_variable
      not_empty_;
  alignas(std::hardware_destructive_interference_size)
      std::condition_variable not_full_;
  alignas(std::hardware_destructive_interference_size) std::mutex mutex_;
};

template <typename T, std::size_t NumSlots>
constexpr bool SpscQueue<T, NumSlots>::TryEnqueueUnsignalled(T value) {
  const auto head = head_.load(std::memory_order_acquire);
  const auto tail = tail_.load(std::memory_order_relaxed);
  const auto new_tail = (tail + 1) % (NumSlots + 1);

  if (new_tail == head) {
    // Queue is full, return false
    return false;
  } else {
    slots_[tail] = std::move(value);
    tail_.store(new_tail, std::memory_order_release);
    return true;
  }
}

template <typename T, std::size_t NumSlots>
constexpr bool SpscQueue<T, NumSlots>::TryEnqueue(T value) {
  const bool success = TryEnqueueUnsignalled(std::move(value));
  if (success) {
    std::lock_guard<std::mutex> lock{mutex_};
    not_empty_.notify_one();
  }
  return success;
}

template <typename T, std::size_t NumSlots>
constexpr void SpscQueue<T, NumSlots>::Enqueue(T value) {
  // Fast path: spin for a bit
  for (const auto i : std::ranges::views::iota(0, 1000)) {
    // We can't use TryEnqueueUnsignalled because it consumes the value, so
    // inline the logic
    const auto head = head_.load(std::memory_order_acquire);
    const auto tail = tail_.load(std::memory_order_relaxed);
    const auto new_tail = (tail + 1) % (NumSlots + 1);

    if (new_tail != head) {
      // Space available—now we can move
      slots_[tail] = std::move(value);
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
    const auto head = head_.load(std::memory_order_acquire);
    const auto tail = tail_.load(std::memory_order_relaxed);
    return (tail + 1) % (NumSlots + 1) != head;
  });

  // Now we have space—move the value in
  const auto tail = tail_.load(std::memory_order_relaxed);
  const auto new_tail = (tail + 1) % (NumSlots + 1);
  slots_[tail] = std::move(value);
  tail_.store(new_tail, std::memory_order_release);
  not_empty_.notify_one();
}

template <typename T, std::size_t NumSlots>
constexpr std::optional<T> SpscQueue<T, NumSlots>::TryDequeueUnsignalled() {
  const auto head = head_.load(std::memory_order_relaxed);
  const auto tail = tail_.load(std::memory_order_acquire);

  if (head == tail) {
    // Queue is empty, return false
    return std::nullopt;
  } else {
    const auto new_head = (head + 1) % (NumSlots + 1);
    auto result = std::move(slots_[head]);
    head_.store(new_head, std::memory_order_release);
    return result;
  }
}

template <typename T, std::size_t NumSlots>
constexpr std::optional<T> SpscQueue<T, NumSlots>::TryDequeue() {
  auto result = TryDequeueUnsignalled();
  if (result.has_value()) {
    std::lock_guard<std::mutex> lock{mutex_};
    not_full_.notify_one();
  }
  return result;
}

template <typename T, std::size_t NumSlots>
constexpr T SpscQueue<T, NumSlots>::Dequeue() {
  // Fast path: spin for a bit
  for (const auto i : std::ranges::views::iota(0, 1000)) {
    auto result = TryDequeueUnsignalled();
    if (result.has_value()) {
      not_full_.notify_one();
      return std::move(*result);
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

#endif  // JGLOCKFREE_SPSC_H
