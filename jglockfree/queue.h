// SPDX-License-Identifier: MIT

#ifndef JGLOCKFREE_INCLUDE_QUEUE_H_
#define JGLOCKFREE_INCLUDE_QUEUE_H_

#include <atomic>
#include <new>
#include <optional>

#include "hazard_pointer.h"

namespace jglockfree {

template <typename Node>
class FreeList {
public:
  FreeList() = default;
  ~FreeList() noexcept;

  FreeList(const FreeList &) = delete;
  FreeList &operator=(const FreeList &) = delete;

  FreeList(FreeList &&) = delete;
  FreeList &operator=(FreeList &&) = delete;

  auto Push(Node *node) -> void;
  auto Pop() -> Node *;
private:
  alignas(std::hardware_destructive_interference_size) std::atomic<Node *> head_{nullptr};
};

template <typename Node>
FreeList<Node>::~FreeList() noexcept {
  auto current = head_.load(std::memory_order_relaxed);
  while (current != nullptr) {
    auto next = current->next.load(std::memory_order_relaxed);
    delete current;
    current = next;
  }
}

template <typename Node>
void FreeList<Node>::Push(Node *node) {
  auto old_head = head_.load(std::memory_order_relaxed);
  do {
    node->next.store(old_head, std::memory_order_relaxed);
  } while (not head_.compare_exchange_weak(old_head, node,
                                           std::memory_order_release,
                                           std::memory_order_relaxed));
}

template <typename Node>
Node *FreeList<Node>::Pop() {
  auto old_head = head_.load(std::memory_order_relaxed);
  Node *next{nullptr};
  do {
    if (old_head == nullptr) {
      return nullptr;
    } else {
      next = old_head->next.load(std::memory_order_relaxed);
    }
  } while (not head_.compare_exchange_weak(old_head, next, std::memory_order_release, std::memory_order_relaxed));
  return old_head;
}

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
  [[nodiscard]] auto Dequeue() noexcept -> std::optional<T>;

 private:
  struct Node {
    std::optional<T> value;
    std::atomic<Node *> next;

    constexpr Node() noexcept : value(std::nullopt), next(nullptr) {}
    explicit constexpr Node(T value) noexcept
        : value(std::move(value)), next(nullptr) {}
  };

  auto AllocateNode(T value) -> Node *;
  static auto RecycleNode(void *ptr) -> void;

  alignas(
      std::hardware_destructive_interference_size) std::atomic<Node *> head_;
  alignas(
      std::hardware_destructive_interference_size) std::atomic<Node *> tail_;
  static thread_local FreeList<Node> free_list_;
};

template <typename T>
thread_local FreeList<typename Queue<T>::Node> Queue<T>::free_list_{};

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
  auto node = AllocateNode(std::move(value));

  while (true) {
    auto *old_tail_ptr = hp_tail.Protect(tail_);
    auto *next = old_tail_ptr->next.load(std::memory_order_acquire);

    if ( old_tail_ptr == tail_.load(std::memory_order_relaxed)) [[likely]] {
      if (next == nullptr) [[likely]] {
        // Try to link new node
        if (old_tail_ptr->next.compare_exchange_weak(
                next, node, std::memory_order_release,
                std::memory_order_relaxed)) [[likely]] {
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

    if (old_head_ptr == head_.load(std::memory_order_acquire)) [[likely]] {
      if (old_head_ptr == old_tail_ptr) [[unlikely]] {
        if (next_ptr == nullptr) {
          hp_head.Clear();
          hp_next.Clear();
          return std::nullopt;
        }
        tail_.compare_exchange_weak(old_tail_ptr, next_ptr,
                                    std::memory_order_release,
                                    std::memory_order_relaxed);
      } else {
        if (head_.compare_exchange_weak(old_head_ptr, next_ptr,
                                        std::memory_order_release,
                                        std::memory_order_relaxed)) [[likely]] {
          auto value = std::move(next_ptr->value);
          hp_head.Retire(old_head_ptr, RecycleNode);
          hp_head.Clear();
          hp_next.Clear();
          return value;
        }
      }
    }
  }
}

template <typename T>
Queue<T>::Node *Queue<T>::AllocateNode(T value) {
 auto *node = free_list_.Pop();
  if (node != nullptr) {
    std::construct_at(&node->value, std::move(value));
  } else {
    node = new Node(std::move(value));
  }
  node->next.store(nullptr, std::memory_order_relaxed);
  return node;
}

template <typename T>
void Queue<T>::RecycleNode(void *ptr) {
  auto node = static_cast<Node *>(ptr);
  std::destroy_at(&node->value);
  free_list_.Push(node);
}

}  // namespace jglockfree

#endif  // JGLOCKFREE_INCLUDE_QUEUE_H_