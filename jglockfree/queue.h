// SPDX-License-Identifier

#ifndef JGLOCKFREE_INCLUDE_QUEUE_H_
#define JGLOCKFREE_INCLUDE_QUEUE_H_

#include <atomic>
#include <cstdint>
#include <optional>

namespace jglockfree {

template <typename T>
class Queue {
public:
  Queue() {
    auto dummy = new Node{};
    head_.store(Pack(dummy, 0), std::memory_order_relaxed);
    tail_.store(Pack(dummy, 0), std::memory_order_relaxed);
  }
  ~Queue() {
    auto current = GetPtr(head_.load(std::memory_order_relaxed));
    while (current != nullptr) {
      auto next = GetPtr(current->next.load(std::memory_order_relaxed));
      delete current;
      current = next;
    }
  }

  void Enqueue(const T value) {
    auto node = new Node(std::move(value));

    for (;;) {
      auto old_tail = tail_.load(std::memory_order_relaxed);
      Node *old_tail_ptr  = GetPtr(old_tail);
      auto next = old_tail_ptr->next.load(std::memory_order_acquire);

      if (old_tail == tail_.load(std::memory_order_relaxed)) {
        if (GetPtr(next) == nullptr) {
          // Step 5: try to link new node
          if (old_tail_ptr->next.compare_exchange_weak(
            next,
            Pack(node, GetTag(next) + 1),
            std::memory_order_release,
            std::memory_order_relaxed)) {
            // Step 6: try to swing Tail forward
             tail_.compare_exchange_weak(
                old_tail,
                Pack(node, GetTag(old_tail) + 1),
                std::memory_order_release,
                std::memory_order_relaxed
                );
            return;
          }
        } else {
          // Step 7: Tail is lagging, help advance it
          tail_.compare_exchange_weak(
              old_tail,
              Pack(GetPtr(next), GetTag(old_tail) + 1),
              std::memory_order_release,
              std::memory_order_relaxed
              );
        }
      }
    }
  }
  std::optional<T> Dequeue() {
    for (;;) {
      auto old_head = head_.load(std::memory_order_relaxed);
      auto old_tail = tail_.load(std::memory_order_relaxed);
      auto *old_head_ptr = GetPtr(old_head);
      auto next = old_head_ptr->next.load(std::memory_order_acquire);

      if (old_head == head_.load(std::memory_order_acquire)) {
        if (old_head == old_tail) {
          if (GetPtr(next) == nullptr) {
            return std::nullopt;
          }
            tail_.compare_exchange_weak(old_tail, Pack(GetPtr(next), GetTag(old_tail) + 1), std::memory_order_release, std::memory_order_relaxed);
        } else {
          auto value = GetPtr(next)->value;
          if (head_.compare_exchange_weak(old_head, Pack(GetPtr(next), GetTag(old_head) + 1), std::memory_order_release, std::memory_order_relaxed)) {
            delete old_head_ptr;
            return value;
          }
        }
      }
    }
  }

private:
  struct Node {
    T value;
    std::atomic<std::uint64_t> next;

    constexpr Node() : next(0) {}
    explicit constexpr Node(T value) : value(std::move(value)), next(0) {}
  };

  // tagged pointer: upper 16 bits are the tag, lower 48 bits are the pointer
  static constexpr std::uint64_t Pack(const Node *ptr, const std::uint64_t tag) {
    return tag << 48 | std::bit_cast<std::uint64_t>(ptr) & 0xFFFF'FFFF'FFFF;
  }

  static constexpr Node *GetPtr(const std::uint64_t tagged) {
    return std::bit_cast<Node *>(tagged & 0xFFFF'FFFF'FFFF);
  }

  static constexpr std::uint16_t GetTag(const std::uint64_t tagged) {
    return static_cast<std::uint16_t>(tagged >> 48);
  }

  std::atomic<std::uint64_t> head_;
  std::atomic<std::uint64_t> tail_;
};

} // namespace jglockfree

#endif // JGLOCKFREE_INCLUDE_QUEUE_H_