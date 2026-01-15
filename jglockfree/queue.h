// SPDX-License-Identifier: MIT

#ifndef JGLOCKFREE_INCLUDE_QUEUE_H_
#define JGLOCKFREE_INCLUDE_QUEUE_H_

#include <atomic>
#include <optional>

#include <jglockfree/config.h>
#include <jglockfree/hazard_pointer.h>

namespace jglockfree {

/**
 * @brief
 * @details
 *
 * @tparam Node The type of nodes stored in the free list.
 */
template <typename Node, typename Traits = DefaultTraits>
class FreeList {
public:
  FreeList() = default;
  ~FreeList() noexcept;

  FreeList(const FreeList &) = delete;
  FreeList &operator=(const FreeList &) = delete;

  FreeList(FreeList &&) = delete;
  FreeList &operator=(FreeList &&) = delete;

  /**
   * @brief
   * @details
   *
   * @param node The node to push onto the free list.
   */
  auto Push(Node *node) -> void;

  /**
   * @brief
   * @details
   *
   * @return A pointer to the popped node, or nullptr if the free list is empty.
   */
  auto Pop() -> Node *;
private:
  alignas(Traits::kCacheLineSize) std::atomic<Node *> head_{nullptr};
};

template <typename Node, typename Traits>
FreeList<Node, Traits>::~FreeList() noexcept {
  auto current = head_.load(std::memory_order_relaxed);
  while (current != nullptr) {
    auto next = current->next.load(std::memory_order_relaxed);
    delete current;
    current = next;
  }
}

template <typename Node, typename Traits>
void FreeList<Node, Traits>::Push(Node *node) {
  auto old_head = head_.load(std::memory_order_relaxed);
  do {
    node->next.store(old_head, std::memory_order_relaxed);
  } while (not head_.compare_exchange_weak(old_head, node,
                                           std::memory_order_release,
                                           std::memory_order_relaxed));
}

template <typename Node, typename Traits>
Node *FreeList<Node, Traits>::Pop() {
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

/**
 * @brief
 * @details
 *
 * @tparam T The type of elements stored in the queue.
 */
template <typename T, typename Traits = DefaultTraits>
class Queue {
 public:
  Queue();
  ~Queue() noexcept;

  Queue(const Queue &) = delete;
  Queue &operator=(const Queue &) = delete;

  Queue(Queue &&) = delete;
  Queue &operator=(Queue &&) = delete;

  /**
   * @brief
   * @details
   *
   * @param value The value to enqueue.
   */
  auto Enqueue(T value) -> void;

  /**
   * @brief
   * @details
   *
   * @return An optional containing the dequeued value if successful, or
   *         std::nullopt if the queue was empty.
   */
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
  static thread_local FreeList<Node, Traits> free_list_;
};

template <typename T, typename Traits>
thread_local FreeList<typename Queue<T, Traits>::Node, Traits> Queue<T, Traits>::free_list_{};

template <typename T, typename Traits>
Queue<T, Traits>::Queue() {
  auto dummy = new Node{};
  head_.store(dummy, std::memory_order_relaxed);
  tail_.store(dummy, std::memory_order_relaxed);
}

template <typename T, typename Traits>
Queue<T, Traits>::~Queue() noexcept {
  auto current = head_.load(std::memory_order_relaxed);
  while (current != nullptr) {
    auto next = current->next.load(std::memory_order_relaxed);
    delete current;
    current = next;
  }
}

template <typename T, typename Traits>
void Queue<T, Traits>::Enqueue(T value) {
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

template <typename T, typename Traits>
std::optional<T> Queue<T, Traits>::Dequeue() noexcept {
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

template <typename T, typename Traits>
Queue<T, Traits>::Node *Queue<T, Traits>::AllocateNode(T value) {
 auto *node = free_list_.Pop();
  if (node != nullptr) {
    std::construct_at(&node->value, std::move(value));
  } else {
    node = new Node(std::move(value));
  }
  node->next.store(nullptr, std::memory_order_relaxed);
  return node;
}

template <typename T, typename Traits>
void Queue<T, Traits>::RecycleNode(void *ptr) {
  auto node = static_cast<Node *>(ptr);
  std::destroy_at(&node->value);
  free_list_.Push(node);
}

}  // namespace jglockfree

#endif  // JGLOCKFREE_INCLUDE_QUEUE_H_