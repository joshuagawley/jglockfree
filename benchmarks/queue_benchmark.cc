#include <benchmark/benchmark.h>
#include <jglockfree/queue.h>
#include <jglockfree/spsc.h>

#include <chrono>
#include <barrier>

#include "mutex_queue.h"

class QueueFixture : public benchmark::Fixture {
 public:
  jglockfree::Queue<int> lock_free_queue;
  MutexQueue<int> mutex_queue;

  void SetUp(const benchmark::State &state) override {
    if (state.thread_index() == 0) {
      for (int i = 0; i < 10'000; ++i) {
        lock_free_queue.Enqueue(i);
        mutex_queue.Enqueue(i);
      }
    }
  }
};

class SpscFixture : public benchmark::Fixture {
 public:
  static constexpr std::size_t kQueueSize = 1024;
  jglockfree::SpscQueue<int, kQueueSize> spsc_queue;
  std::unique_ptr<std::barrier<>> sync_barrier;

  void SetUp(const benchmark::State &state) override {
    if (state.thread_index() == 0) {
      sync_barrier = std::make_unique<std::barrier<>>(2);
    }
  }

  void TearDown(const benchmark::State &state) override {
    if (state.thread_index() == 0) {
      // Drain any remaining items
      while (spsc_queue.TryDequeue().has_value()) {
      }
    }
  }
};

// clang-format off
BENCHMARK_DEFINE_F(QueueFixture, LockFreeEnqueue)(benchmark::State &state) {
// clang-format on
  for (auto _ : state) {
    lock_free_queue.Enqueue(42);
  }
}

// clang-format off
BENCHMARK_DEFINE_F(QueueFixture, MutexEnqueue)(benchmark::State &state) {
// clang-format on
  for (auto _ : state) {
    mutex_queue.Enqueue(42);
  }
}

// clang-format off
BENCHMARK_DEFINE_F(QueueFixture, LockFreeMixed)(benchmark::State &state) {
// clang-format on
  for (auto _ : state) {
    if (state.thread_index() % 2 == 0) {
      lock_free_queue.Enqueue(42);
    } else {
      benchmark::DoNotOptimize(lock_free_queue.Dequeue());
    }
  }
}

// clang-format off
BENCHMARK_DEFINE_F(QueueFixture, MutexMixed)(benchmark::State &state) {
// clang-format on
  for (auto _ : state) {
    if (state.thread_index() % 2 == 0) {
      mutex_queue.Enqueue(42);
    } else {
      benchmark::DoNotOptimize(mutex_queue.Dequeue());
    }
  }
}

// clang-format off
BENCHMARK_DEFINE_F(QueueFixture, LockFreeThroughput)(benchmark::State &state) {
// clang-format on
  if (state.threads() != 2) {
    state.SkipWithError("Throughput benchmark requires exactly 2 threads");
    return;
  }

  constexpr std::size_t kItemsPerIteration = 10000;

  for (auto _ : state) {
    if (state.thread_index() == 0) {
      // Producer: enqueue all items
      for (std::size_t i = 0; i < kItemsPerIteration; ++i) {
        lock_free_queue.Enqueue(i);
      }
    } else {
      // Consumer: dequeue all items
      for (std::size_t i = 0; i < kItemsPerIteration; ++i) {
        while (not lock_free_queue.Dequeue().has_value()) {
          // Spin if empty
        }
      }
    }
  }

  state.SetItemsProcessed(state.iterations() * kItemsPerIteration);
}

// clang-format off
BENCHMARK_DEFINE_F(QueueFixture, MutexThroughput)(benchmark::State &state) {
// clang-format on
  if (state.threads() != 2) {
    state.SkipWithError("Throughput benchmark requires exactly 2 threads");
    return;
  }

  constexpr std::size_t kItemsPerIteration = 10000;

  for (auto _ : state) {
    if (state.thread_index() == 0) {
      // Producer: enqueue all items
      for (std::size_t i = 0; i < kItemsPerIteration; ++i) {
        mutex_queue.Enqueue(i);
      }
    } else {
      // Consumer: dequeue all items
      for (std::size_t i = 0; i < kItemsPerIteration; ++i) {
        while (not mutex_queue.Dequeue().has_value()) {
          // Spin if empty
        }
      }
    }
  }

  state.SetItemsProcessed(state.iterations() * kItemsPerIteration);
}

// clang-format off
BENCHMARK_DEFINE_F(QueueFixture, LockFreeLatencyDistribution)(benchmark::State &state) {
// clang-format on
  std::vector<int64_t> latencies;
  latencies.reserve(state.max_iterations);

  for (auto _ : state) {
    state.PauseTiming();
    auto t0 = std::chrono::high_resolution_clock::now();
    state.ResumeTiming();

    lock_free_queue.Enqueue(42);
    benchmark::DoNotOptimize(lock_free_queue.Dequeue());

    state.PauseTiming();
    auto t1 = std::chrono::high_resolution_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    latencies.push_back(ns);
    state.ResumeTiming();
  }

  if (state.thread_index() == 0 && !latencies.empty()) {
    std::ranges::sort(latencies);

    auto percentile = [&](double p) -> double {
      const auto idx = static_cast<std::size_t>(p * static_cast<double>(latencies.size() - 1));
      return static_cast<double>(latencies[idx]);
    };

    state.counters["p50_ns"] = percentile(0.50);
    state.counters["p90_ns"] = percentile(0.90);
    state.counters["p99_ns"] = percentile(0.99);
    state.counters["p999_ns"] = percentile(0.999);
    state.counters["max_ns"] = static_cast<double>(latencies.back());
  }
}

// clang-format off
BENCHMARK_DEFINE_F(QueueFixture, MutexLatencyDistribution)(benchmark::State &state) {
// clang-format on
  std::vector<int64_t> latencies;
  latencies.reserve(state.max_iterations);

  for (auto _ : state) {
    state.PauseTiming();
    auto t0 = std::chrono::high_resolution_clock::now();
    state.ResumeTiming();

    mutex_queue.Enqueue(42);
    benchmark::DoNotOptimize(mutex_queue.Dequeue());

    state.PauseTiming();
    auto t1 = std::chrono::high_resolution_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    latencies.push_back(ns);
    state.ResumeTiming();
  }

  if (state.thread_index() == 0 && !latencies.empty()) {
    std::ranges::sort(latencies);

    auto percentile = [&](double p) -> double {
      auto idx = static_cast<std::size_t>(p * static_cast<double>(latencies.size() - 1));
      return static_cast<double>(latencies[idx]);
    };

    state.counters["p50_ns"] = percentile(0.50);
    state.counters["p90_ns"] = percentile(0.90);
    state.counters["p99_ns"] = percentile(0.99);
    state.counters["p999_ns"] = percentile(0.999);
    state.counters["max_ns"] = static_cast<double>(latencies.back());
  }
}


// clang-format off
BENCHMARK_DEFINE_F(SpscFixture, SpscTryPaired)(benchmark::State &state) {
// clang-format on
  // Ensure exactly 2 threads
  if (state.threads() != 2) {
    state.SkipWithError("SPSC requires exactly 2 threads");
    return;
  }

  for (auto _ : state) {
    if (state.thread_index() == 0) {
      // Producer: enqueue then signal
      spsc_queue.TryEnqueue(42);
      sync_barrier->arrive_and_wait();  // Item is ready
      sync_barrier->arrive_and_wait();  // Wait for consumer
    } else {
      // Consumer: wait for item then dequeue
      sync_barrier->arrive_and_wait();  // Wait for item
      benchmark::DoNotOptimize(spsc_queue.TryDequeue());
      sync_barrier->arrive_and_wait();  // Signal done
    }
  }
}

// clang-format off
BENCHMARK_DEFINE_F(SpscFixture, SpscBlockingPaired)(benchmark::State &state) {
// clang-format on
  if (state.threads() != 2) {
    state.SkipWithError("SPSC requires exactly 2 threads");
    return;
  }

  for (auto _ : state) {
    if (state.thread_index() == 0) {
      // Producer: enqueue then signal
      spsc_queue.Enqueue(42);
      sync_barrier->arrive_and_wait();  // Item is ready
      sync_barrier->arrive_and_wait();  // Wait for consumer
    } else {
      // Consumer: wait for item then dequeue
      sync_barrier->arrive_and_wait();  // Wait for item
      benchmark::DoNotOptimize(spsc_queue.Dequeue());
      sync_barrier->arrive_and_wait();  // Signal done
    }
  }
}

// ============================================================================
// Individual Enqueue Operations
//
// Pre-drain the queue so there's always space. Only the producer does work;
// the consumer just synchronises but doesn't dequeue.
// ============================================================================

// clang-format off
BENCHMARK_DEFINE_F(SpscFixture, SpscTryEnqueueOnly)(benchmark::State &state) {
// clang-format on
  if (state.threads() != 2) {
    state.SkipWithError("SPSC requires exactly 2 threads");
    return;
  }

  for (auto _ : state) {
    if (state.thread_index() == 0) {
      // Producer: enqueue
      benchmark::DoNotOptimize(spsc_queue.TryEnqueue(42));
      sync_barrier->arrive_and_wait();
    } else {
      // Consumer: just drain to make space, don't measure
      state.PauseTiming();
      while (spsc_queue.TryDequeue().has_value()) {
      }
      state.ResumeTiming();
      sync_barrier->arrive_and_wait();
    }
  }
}

// clang-format off
BENCHMARK_DEFINE_F(SpscFixture, SpscBlockingEnqueueOnly)(benchmark::State &state) {
// clang-format on
  if (state.threads() != 2) {
    state.SkipWithError("SPSC requires exactly 2 threads");
    return;
  }

  for (auto _ : state) {
    if (state.thread_index() == 0) {
      // Producer: enqueue
      spsc_queue.Enqueue(42);
      sync_barrier->arrive_and_wait();
    } else {
      // Consumer: just drain to make space, don't measure
      state.PauseTiming();
      while (spsc_queue.TryDequeue().has_value()) {
      }
      state.ResumeTiming();
      sync_barrier->arrive_and_wait();
    }
  }
}

// ============================================================================
// Individual Dequeue Operations
//
// Pre-fill the queue so there's always an item. Only the consumer does work;
// the producer just synchronises and refills.
// ============================================================================

// clang-format off
BENCHMARK_DEFINE_F(SpscFixture, SpscTryDequeueOnly)(benchmark::State &state) {
// clang-format on
  if (state.threads() != 2) {
    state.SkipWithError("SPSC requires exactly 2 threads");
    return;
  }

  // Pre-fill with one item before starting
  if (state.thread_index() == 0) {
    spsc_queue.TryEnqueue(42);
  }

  for (auto _ : state) {
    if (state.thread_index() == 0) {
      // Producer: just refill, don't measure
      state.PauseTiming();
      spsc_queue.TryEnqueue(42);
      state.ResumeTiming();
      sync_barrier->arrive_and_wait();
    } else {
      // Consumer: dequeue
      benchmark::DoNotOptimize(spsc_queue.TryDequeue());
      sync_barrier->arrive_and_wait();
    }
  }
}

// clang-format off
BENCHMARK_DEFINE_F(SpscFixture, SpscBlockingDequeueOnly)(benchmark::State &state) {
// clang-format on
  if (state.threads() != 2) {
    state.SkipWithError("SPSC requires exactly 2 threads");
    return;
  }

  // Pre-fill with one item before starting
  if (state.thread_index() == 0) {
    spsc_queue.TryEnqueue(42);
  }

  for (auto _ : state) {
    if (state.thread_index() == 0) {
      // Producer: just refill, don't measure
      state.PauseTiming();
      spsc_queue.TryEnqueue(42);
      state.ResumeTiming();
      sync_barrier->arrive_and_wait();
    } else {
      // Consumer: dequeue
      benchmark::DoNotOptimize(spsc_queue.Dequeue());
      sync_barrier->arrive_and_wait();
    }
  }
}

// ============================================================================
// Throughput Benchmark
//
// Measures sustained throughput: producer enqueues N items, consumer dequeues
// N items, measure total time. This captures realistic pipelining behaviour.
// ============================================================================

// clang-format off
BENCHMARK_DEFINE_F(SpscFixture, SpscThroughput)(benchmark::State &state) {
// clang-format on
  if (state.threads() != 2) {
    state.SkipWithError("SPSC requires exactly 2 threads");
    return;
  }

  constexpr std::size_t kItemsPerIteration = 10000;

  for (auto _ : state) {
    // sync_barrier->arrive_and_wait();  // Start together

    if (state.thread_index() == 0) {
      // Producer: enqueue all items
      for (std::size_t i = 0; i < kItemsPerIteration; ++i) {
        while (not spsc_queue.TryEnqueue(i)) {
          // Spin if full
        }
      }
    } else {
      // Consumer: dequeue all items
      for (std::size_t i = 0; i < kItemsPerIteration; ++i) {
        while (not spsc_queue.TryDequeue().has_value()) {
          // Spin if empty
        }
      }
    }

    // sync_barrier->arrive_and_wait();  // End together
  }

  // Report throughput
  state.SetItemsProcessed(state.iterations() * kItemsPerIteration);
}

// clang-format off
BENCHMARK_DEFINE_F(SpscFixture, SpscThroughputInternal)(benchmark::State &state) {
// clang-format on
  if (state.threads() != 2) {
    state.SkipWithError("SPSC requires exactly 2 threads");
    return;
  }

  constexpr std::size_t kItemsPerIteration = 10000;

  for (auto _ : state) {
    if (state.thread_index() == 0) {
      // Producer: enqueue all items using internal method (no signalling)
      for (std::size_t i = 0; i < kItemsPerIteration; ++i) {
        while (not spsc_queue.TryEnqueueUnsignalled(i)) {
          // Spin if full
        }
      }
    } else {
      // Consumer: dequeue all items using internal method (no signalling)
      for (std::size_t i = 0; i < kItemsPerIteration; ++i) {
        while (not spsc_queue.TryDequeueUnsignalled().has_value()) {
          // Spin if empty
        }
      }
    }
  }

  state.SetItemsProcessed(state.iterations() * kItemsPerIteration);
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
BENCHMARK_REGISTER_F(QueueFixture, LockFreeLatencyDistribution)
    ->Threads(1)
    ->Threads(2)
    ->Threads(4)
    ->Threads(8)
    ->Iterations(100000)
    ->Unit(benchmark::kNanosecond);
BENCHMARK_REGISTER_F(QueueFixture, MutexLatencyDistribution)
    ->Threads(1)
    ->Threads(2)
    ->Threads(4)
    ->Threads(8)
    ->Iterations(100000)
    ->Unit(benchmark::kNanosecond);

BENCHMARK_REGISTER_F(SpscFixture, SpscTryPaired)->Threads(2);
BENCHMARK_REGISTER_F(SpscFixture, SpscBlockingPaired)->Threads(2);
BENCHMARK_REGISTER_F(SpscFixture, SpscTryEnqueueOnly)->Threads(2);
BENCHMARK_REGISTER_F(SpscFixture, SpscBlockingEnqueueOnly)->Threads(2);
BENCHMARK_REGISTER_F(SpscFixture, SpscTryDequeueOnly)->Threads(2);
BENCHMARK_REGISTER_F(SpscFixture, SpscBlockingDequeueOnly)->Threads(2);

BENCHMARK_REGISTER_F(QueueFixture, LockFreeThroughput)->Threads(2);
BENCHMARK_REGISTER_F(QueueFixture, MutexThroughput)->Threads(2);
BENCHMARK_REGISTER_F(SpscFixture, SpscThroughput)->Threads(2);
BENCHMARK_REGISTER_F(SpscFixture, SpscThroughputInternal)->Threads(2);
