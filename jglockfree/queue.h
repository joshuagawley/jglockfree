// SPDX-License-Identifier: MIT

#ifndef JGLOCKFREE_INCLUDE_QUEUE_H_
#define JGLOCKFREE_INCLUDE_QUEUE_H_

#include <atomic>
#include <new>
#include <optional>

#include "hazard_pointer.h"

namespace jglockfree {

template <typename T>
class Queue {
 public:
  Queue();
  ~Queue() noexcept;

  Queue(const Queue &) = delete;
  Queue &operator=(const Queue &) = delete;

  Queue(Queue &&) = delete;
  Queue &operator=(Queue &&) = delete;

  auto Enqueue(T value) -> void;
  auto Dequeue() noexcept -> std::optional<T>;

 private:
  struct Node {
    T value;
    std::atomic<Node *> next;

    constexpr Node() noexcept : next(nullptr) {}
    explicit constexpr Node(T value) noexcept
        : value(std::move(value)), next(nullptr) {}
  };

  alignas(
      std::hardware_destructive_interference_size) std::atomic<Node *> head_;
  alignas(
      std::hardware_destructive_interference_size) std::atomic<Node *> tail_;
};

template <typename T>
Queue<T>::Queue() {
  auto dummy = new Node{};
  head_.store(dummy, std::memory_order_relaxed);
  tail_.store(dummy, std::memory_order_relaxed);
}

template <typename T>
Queue<T>::~Queue() noexcept {
  auto current = head_.load(std::memory_order_relaxed);
  while (current != nullptr) {
    auto next = current->next.load(std::memory_order_relaxed);
    delete current;
    current = next;
  }
}

template <typename T>
void Queue<T>::Enqueue(T value) {
  thread_local HazardPointer hp_tail;
  auto node = new Node(std::move(value));

  while (true) {
    auto *old_tail_ptr = hp_tail.Protect(tail_);
    auto *next = old_tail_ptr->next.load(std::memory_order_acquire);

    if (old_tail_ptr == tail_.load(std::memory_order_relaxed)) {
      if (next == nullptr) {
        // Try to link new node
        if (old_tail_ptr->next.compare_exchange_weak(
                next, node, std::memory_order_release,
                std::memory_order_relaxed)) {
          // Try to swing tail forward
          tail_.compare_exchange_weak(old_tail_ptr, node,
                                      std::memory_order_release,
                                      std::memory_order_relaxed);
          hp_tail.Clear();
          return;
        }
      } else {
        // Tail is lagging, help advance it
        tail_.compare_exchange_weak(old_tail_ptr, next,
                                    std::memory_order_release,
                                    std::memory_order_relaxed);
      }
    }
  }
}

template <typename T>
std::optional<T> Queue<T>::Dequeue() noexcept {
  thread_local HazardPointer hp_head;
  thread_local HazardPointer hp_next;

  while (true) {
    auto *old_head_ptr = hp_head.Protect(head_);
    auto *old_tail_ptr = tail_.load(std::memory_order_relaxed);
    auto *next_ptr = hp_next.Protect(old_head_ptr->next);

    if (old_head_ptr == head_.load(std::memory_order_acquire)) {
      if (old_head_ptr == old_tail_ptr) {
        if (next_ptr == nullptr) {
          hp_head.Clear();
          hp_next.Clear();
          return std::nullopt;
        }
        tail_.compare_exchange_weak(old_tail_ptr, next_ptr,
                                    std::memory_order_release,
                                    std::memory_order_relaxed);
      } else {
        auto value = next_ptr->value;
        if (head_.compare_exchange_weak(old_head_ptr, next_ptr,
                                        std::memory_order_release,
                                        std::memory_order_relaxed)) {
          hp_head.Retire(old_head_ptr);
          hp_head.Clear();
          hp_next.Clear();
          return value;
        }
      }
    }
  }
}

}  // namespace jglockfree

#endif  // JGLOCKFREE_INCLUDE_QUEUE_H_