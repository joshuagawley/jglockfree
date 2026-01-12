#ifndef JGLOCKFREE_MUTEX_QUEUE_H
#define JGLOCKFREE_MUTEX_QUEUE_H

#include <mutex>
#include <optional>
#include <queue>

template <typename T>
class MutexQueue {
 public:
   MutexQueue() = default;
   ~MutexQueue() = default;

   auto Enqueue(T value) -> void;
  [[nodiscard]] auto Dequeue() -> std::optional<T>;

 private:
  std::queue<T> queue_;
  std::mutex mutex_;
};

template <typename T>
void MutexQueue<T>::Enqueue(T value) {
  std::lock_guard lock(mutex_);
  queue_.push(std::move(value));
}

template <typename T>
std::optional<T> MutexQueue<T>::Dequeue() {
  std::lock_guard lock(mutex_);
  if (queue_.empty()) {
    return std::nullopt;
  }
  T value = std::move(queue_.front());
  queue_.pop();
  return value;
}

#endif  // JGLOCKFREE_MUTEX_QUEUE_H