#include <benchmark/benchmark.h>
#include <jglockfree/queue.h>

#include <latch>
#include <ranges>

#include "mutex_queue.h"

class QueueFixture : public benchmark::Fixture {
 public:
  jglockfree::Queue<int> lock_free_queue;
  MutexQueue<int> mutex_queue;

  void SetUp(const benchmark::State &state) override {
    if (state.thread_index() == 0) {
      std::ranges::for_each(std::views::iota(0, 10'000), [&](int i) {
        lock_free_queue.Enqueue(i);
        mutex_queue.Enqueue(i);
      });
    }
  }
};

BENCHMARK_DEFINE_F(QueueFixture, LockFreeEnqueue)(benchmark::State &state) {
  for (auto _ : state) {
    lock_free_queue.Enqueue(42);
  }
}

BENCHMARK_DEFINE_F(QueueFixture, MutexEnqueue)(benchmark::State &state) {
  for (auto _ : state) {
    mutex_queue.Enqueue(42);
  }
}

BENCHMARK_DEFINE_F(QueueFixture, LockFreeMixed)(benchmark::State &state) {
  for (auto _ : state) {
    if (state.thread_index() % 2 == 0) {
      lock_free_queue.Enqueue(42);
    } else {
      benchmark::DoNotOptimize(lock_free_queue.Dequeue());
    }
  }
}

BENCHMARK_DEFINE_F(QueueFixture, MutexMixed)(benchmark::State &state) {
  for (auto _ : state) {
    if (state.thread_index() % 2 == 0) {
      mutex_queue.Enqueue(42);
    } else {
      benchmark::DoNotOptimize(mutex_queue.Dequeue());
    }
  }
}

BENCHMARK_REGISTER_F(QueueFixture, LockFreeEnqueue)
    ->Threads(1)
    ->Threads(2)
    ->Threads(4)
    ->Threads(8);
BENCHMARK_REGISTER_F(QueueFixture, MutexEnqueue)
    ->Threads(1)
    ->Threads(2)
    ->Threads(4)
    ->Threads(8);
BENCHMARK_REGISTER_F(QueueFixture, LockFreeMixed)
    ->Threads(1)
    ->Threads(2)
    ->Threads(4)
    ->Threads(8);
BENCHMARK_REGISTER_F(QueueFixture, MutexMixed)
    ->Threads(1)
    ->Threads(2)
    ->Threads(4)
    ->Threads(8);
