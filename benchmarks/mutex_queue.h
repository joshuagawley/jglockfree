#ifndef JGLOCKFREE_MUTEX_QUEUE_H
#define JGLOCKFREE_MUTEX_QUEUE_H

#include <mutex>
#include <optional>
#include <queue>

template <typename T>
class MutexQueue {
public:
  void Enqueue(T value) {
    std::lock_guard lock(mutex_);
    queue_.push(std::move(value));
  }

  std::optional<T> Dequeue() {
    std::lock_guard lock(mutex_);
    if (queue_.empty()) {
      return std::nullopt;
    }
    T value = std::move(queue_.front());
    queue_.pop();
    return value;
  }
private:
  std::queue<T> queue_;
  std::mutex mutex_;
};

#endif //JGLOCKFREE_MUTEX_QUEUE_H
