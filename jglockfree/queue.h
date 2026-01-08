// SPDX-License-Identifier: MIT

#ifndef JGLOCKFREE_INCLUDE_QUEUE_H_
#define JGLOCKFREE_INCLUDE_QUEUE_H_

#include "hazard_pointer.h"

#include <atomic>
#include <optional>

namespace jglockfree {

template <typename T>
class Queue {
public:
  Queue() {
    auto dummy = new Node{};
    head_.store(dummy, std::memory_order_relaxed);
    tail_.store(dummy, std::memory_order_relaxed);
  }
  ~Queue() {
    auto current = head_.load(std::memory_order_relaxed);
    while (current != nullptr) {
      auto next = current->next.load(std::memory_order_relaxed);
      delete current;
      current = next;
    }
  }

  void Enqueue(const T value) {
    thread_local HazardPointer hp_tail;
    auto node = new Node(std::move(value));

    while (true) {
      auto *old_tail_ptr = hp_tail.Protect(tail_);
      auto *next = old_tail_ptr->next.load(std::memory_order_acquire);

      if (old_tail_ptr == tail_.load(std::memory_order_relaxed)) {
        if (next == nullptr) {
          // Step 5: try to link new node
          if (old_tail_ptr->next.compare_exchange_weak(
            next,
            node,
            std::memory_order_release,
            std::memory_order_relaxed)) {
            // Step 6: try to swing Tail forward
             tail_.compare_exchange_weak(
                old_tail_ptr,
                node,
                std::memory_order_release,
                std::memory_order_relaxed
                );
            hp_tail.Clear();
            return;
          }
        } else {
          // Step 7: Tail is lagging, help advance it
          tail_.compare_exchange_weak(
              old_tail_ptr,
              next,
              std::memory_order_release,
              std::memory_order_relaxed
              );
        }
      }
    }
  }
  std::optional<T> Dequeue() {
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
          tail_.compare_exchange_weak(old_tail_ptr, next_ptr, std::memory_order_release, std::memory_order_relaxed);
        } else {
          auto value = next_ptr->value;
          if (head_.compare_exchange_weak(old_head_ptr, next_ptr, std::memory_order_release, std::memory_order_relaxed)) {
            hp_head.Retire(old_head_ptr);
            hp_head.Clear();
            hp_next.Clear();
            return value;
          }
        }
      }
    }
  }

private:
  struct Node {
    T value;
    std::atomic<Node *> next;

    constexpr Node() : next(nullptr) {}
    explicit constexpr Node(T value) : value(std::move(value)), next(nullptr) {}
  };

  std::atomic<Node *> head_;
  std::atomic<Node *> tail_;
};

} // namespace jglockfree

#endif // JGLOCKFREE_INCLUDE_QUEUE_H_