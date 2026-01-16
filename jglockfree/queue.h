// SPDX-License-Identifier: MIT

#ifndef JGLOCKFREE_INCLUDE_QUEUE_H_
#define JGLOCKFREE_INCLUDE_QUEUE_H_

#include <jglockfree/config.h>
#include <jglockfree/hazard_pointer.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace jglockfree {

template <typename Node, typename Traits = DefaultTraits>
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
  [[nodiscard]] auto get_count() const noexcept -> std::size_t {
    return count_;
  }

 private:
  alignas(Traits::kCacheLineSize) std::atomic<Node *> head_{nullptr};
  std::size_t count_{0};
};

template <typename Node, typename Traits>
FreeList<Node, Traits>::~FreeList() noexcept {
  Node *current = head_.load(std::memory_order_relaxed);
  while (current != nullptr) {
    Node *next = current->next.load(std::memory_order_relaxed);
    delete current;
    current = next;
  }
}

template <typename Node, typename Traits>
void FreeList<Node, Traits>::Push(Node *node) {
  auto old_head = head_.load(std::memory_order_relaxed);
  do {
    node->next.store(old_head, std::memory_order_relaxed);
  } while (not head_.compare_exchange_weak(
      old_head, node, std::memory_order_release, std::memory_order_relaxed));
}

template <typename Node, typename Traits>
Node *FreeList<Node, Traits>::Pop() {
  Node *old_head = head_.load(std::memory_order_relaxed);
  Node *next = nullptr;
  do {
    if (old_head == nullptr) {
      return nullptr;
    } else {
      next = old_head->next.load(std::memory_order_relaxed);
    }
  } while (not head_.compare_exchange_weak(
      old_head, next, std::memory_order_release, std::memory_order_relaxed));
  return old_head;
}

template <typename T, typename Traits = DefaultTraits>
  requires std::is_nothrow_move_constructible_v<T>
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

  alignas(Traits::kCacheLineSize) std::atomic<Node *> head_;
  alignas(Traits::kCacheLineSize) std::atomic<Node *> tail_;
  static inline std::vector<Node *> global_free_list_;
  static inline std::mutex global_free_list_guard_{};
  static thread_local FreeList<Node, Traits> free_list_;
};

template <typename T, typename Traits>
  requires std::is_nothrow_move_constructible_v<T>
thread_local FreeList<typename Queue<T, Traits>::Node, Traits>
    Queue<T, Traits>::free_list_{};

template <typename T, typename Traits>
  requires std::is_nothrow_move_constructible_v<T>
Queue<T, Traits>::Queue() {
  auto dummy = new Node{};
  head_.store(dummy, std::memory_order_relaxed);
  tail_.store(dummy, std::memory_order_relaxed);
}

template <typename T, typename Traits>
  requires std::is_nothrow_move_constructible_v<T>
Queue<T, Traits>::~Queue() noexcept {
  auto current = head_.load(std::memory_order_relaxed);
  while (current != nullptr) {
    auto next = current->next.load(std::memory_order_relaxed);
    delete current;
    current = next;
  }
}

template <typename T, typename Traits>
  requires std::is_nothrow_move_constructible_v<T>
void Queue<T, Traits>::Enqueue(T value) {
  thread_local HazardPointer hp_tail;
  Node *node = AllocateNode(std::move(value));

  while (true) {
    Node *old_tail_ptr = hp_tail.Protect(tail_);
    Node *next = old_tail_ptr->next.load(std::memory_order_acquire);

    if (old_tail_ptr == tail_.load(std::memory_order_relaxed)) [[likely]] {
      if (next == nullptr) [[likely]] {
        // Try to link new node
        if (old_tail_ptr->next.compare_exchange_weak(
                next, node, std::memory_order_release,
                std::memory_order_relaxed)) [[likely]] {
          // Try to swing tail forward
          tail_.

              compare_exchange_weak(old_tail_ptr, node,
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

template <typename T, typename Traits>
  requires std::is_nothrow_move_constructible_v<T>
std::optional<T> Queue<T, Traits>::Dequeue() noexcept {
  thread_local HazardPointer hp_head;
  thread_local HazardPointer hp_next;

  while (true) {
    Node *old_head_ptr = hp_head.Protect(head_);
    Node *old_tail_ptr = tail_.load(std::memory_order_relaxed);
    Node *next_ptr = hp_next.Protect(old_head_ptr->next);

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
          std::optional<T> value = std::move(next_ptr->value);
          hp_head.Retire(old_head_ptr, RecycleNode);
          hp_head.Clear();
          hp_next.Clear();
          return value;
        }
      }
    }
  }
}

template <typename T, typename Traits>
  requires std::is_nothrow_move_constructible_v<T>
Queue<T, Traits>::Node *Queue<T, Traits>::AllocateNode(T value) {
  Node *node = free_list_.Pop();
  if (node == nullptr) {
    std::lock_guard lock{global_free_list_guard_};
    constexpr int kBatchSize = 32;
    int items_moved = 0;

    while (items_moved < kBatchSize && global_free_list_.size() > 0) {
      Node *from_global = global_free_list_.back();
      global_free_list_.pop_back();

      free_list_.Push(from_global);
      ++items_moved;
    }

    if (items_moved > 0) {
      node = free_list_.Pop();
    }
  }

  if (node != nullptr) {
    try {
      std::construct_at(&node->value, std::move(value));
    } catch (...) {
      // if constructing the value failed, node->value is still uninitialized
      // so push it back into the free list and rethrow the exception
      free_list_.Push(node);
      throw;
    }
  } else {
    node = new Node(std::move(value));
  }
  node->next.store(nullptr, std::memory_order_relaxed);
  return node;
}

template <typename T, typename Traits>
  requires std::is_nothrow_move_constructible_v<T>
void Queue<T, Traits>::RecycleNode(void *ptr) {
  auto node = static_cast<Node *>(ptr);
  std::destroy_at(&node->value);
  free_list_.Push(node);

  constexpr std::size_t kMaxLocalSize = 64;
  constexpr int kBatchSize = 32;

  if (free_list_.get_count() > kMaxLocalSize) {
    std::lock_guard lock{global_free_list_guard_};

    for (int i = 0; i < kBatchSize; ++i) {
      Node *to_return = free_list_.Pop();
      if (to_return != nullptr) {
        global_free_list_.push_back(to_return);
      }
    }
  }
}

}  // namespace jglockfree

#endif  // JGLOCKFREE_INCLUDE_QUEUE_H_
