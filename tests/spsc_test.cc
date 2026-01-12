// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>
#include <jglockfree/spsc.h>

#include <chrono>
#include <ranges>
#include <thread>
#include <vector>

// Helper: spin-enqueue until successful
template <typename T, std::size_t N>
void SpinEnqueue(jglockfree::SpscQueue<T, N> &queue, T value) {
  while (not queue.TryEnqueue(std::move(value))) {
    std::this_thread::yield();
  }
}

// Helper: spin-dequeue until successful
template <typename T, std::size_t N>
T SpinDequeue(jglockfree::SpscQueue<T, N> &queue) {
  while (true) {
    auto result = queue.TryDequeue();
    if (result.has_value()) {
      return std::move(*result);
    }
    std::this_thread::yield();
  }
}

// ============================================================================
// Basic Single-Threaded Operations
// ============================================================================

TEST(SpscQueueTest, EmptyQueueDequeueReturnsNullopt) {
  jglockfree::SpscQueue<int, 8> queue;
  EXPECT_EQ(queue.TryDequeue(), std::nullopt);
}

TEST(SpscQueueTest, SingleEnqueueDequeue) {
  jglockfree::SpscQueue<int, 8> queue;
  EXPECT_TRUE(queue.TryEnqueue(42));
  const auto result = queue.TryDequeue();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 42);
}

TEST(SpscQueueTest, MultipleEnqueueDequeuePreservesOrder) {
  // FIFO ordering - first in, first out
  jglockfree::SpscQueue<int, 8> queue;
  EXPECT_TRUE(queue.TryEnqueue(1));
  EXPECT_TRUE(queue.TryEnqueue(2));
  EXPECT_TRUE(queue.TryEnqueue(3));

  EXPECT_EQ(queue.TryDequeue(), 1);
  EXPECT_EQ(queue.TryDequeue(), 2);
  EXPECT_EQ(queue.TryDequeue(), 3);
  EXPECT_EQ(queue.TryDequeue(), std::nullopt);
}

TEST(SpscQueueTest, InterleavedEnqueueDequeue) {
  jglockfree::SpscQueue<int, 8> queue;
  EXPECT_TRUE(queue.TryEnqueue(1));
  EXPECT_EQ(queue.TryDequeue(), 1);

  EXPECT_TRUE(queue.TryEnqueue(2));
  EXPECT_TRUE(queue.TryEnqueue(3));
  EXPECT_EQ(queue.TryDequeue(), 2);

  EXPECT_TRUE(queue.TryEnqueue(4));
  EXPECT_EQ(queue.TryDequeue(), 3);
  EXPECT_EQ(queue.TryDequeue(), 4);
  EXPECT_EQ(queue.TryDequeue(), std::nullopt);
}

// ============================================================================
// Capacity and Wraparound
// ============================================================================

TEST(SpscQueueTest, FillToCapacity) {
  // With NumSlots=8, array size is 9, capacity is 8
  jglockfree::SpscQueue<int, 8> queue;

  std::ranges::for_each(std::ranges::views::iota(0, 8), [&queue](int i) {
    EXPECT_TRUE(queue.TryEnqueue(std::move(i)))
        << "Failed to enqueue element " << i;
  });

  // Queue should now be full
  EXPECT_FALSE(queue.TryEnqueue(999)) << "Queue should be full";

  // All 8 should come back out
  std::ranges::for_each(std::ranges::views::iota(0, 8), [&queue](int i) {
    const auto result = queue.TryDequeue();
    ASSERT_TRUE(result.has_value()) << "Failed to dequeue element " << i;
    EXPECT_EQ(*result, i);
  });

  EXPECT_EQ(queue.TryDequeue(), std::nullopt);
}

TEST(SpscQueueTest, WrapAroundBehavior) {
  // Force the indices to wrap around the array boundary
  jglockfree::SpscQueue<int, 4> queue;  // Capacity of 4, array size 5

  // Fill and drain a few times to move head/tail past the array end
  std::ranges::for_each(std::ranges::views::iota(0, 5), [&queue](int round) {
    EXPECT_TRUE(queue.TryEnqueue(round * 10 + 1));
    EXPECT_TRUE(queue.TryEnqueue(round * 10 + 2));
    EXPECT_TRUE(queue.TryEnqueue(round * 10 + 3));

    EXPECT_EQ(queue.TryDequeue(), round * 10 + 1);
    EXPECT_EQ(queue.TryDequeue(), round * 10 + 2);
    EXPECT_EQ(queue.TryDequeue(), round * 10 + 3);
  });
}

TEST(SpscQueueTest, PartialFillDrainWrapAround) {
  jglockfree::SpscQueue<int, 4> queue;  // Capacity of 4

  // Enqueue one, dequeue one - keeps queue at 0-1 items while indices wrap
  std::ranges::for_each(std::ranges::views::iota(0, 5), [&queue](int i) {
    EXPECT_TRUE(queue.TryEnqueue(std::move(i)))
        << "Failed to enqueue at i=" << i;
    const auto result = queue.TryDequeue();
    ASSERT_TRUE(result.has_value()) << "Failed to dequeue at i=" << i;
    EXPECT_EQ(*result, i);
  });
}

// ============================================================================
// Type Handling
// ============================================================================

TEST(SpscQueueTest, WorksWithMoveOnlyTypes) {
  jglockfree::SpscQueue<std::unique_ptr<int>, 4> queue;

  EXPECT_TRUE(queue.TryEnqueue(std::make_unique<int>(42)));
  const auto result = queue.TryDequeue();

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(**result, 42);
}

TEST(SpscQueueTest, WorksWithStrings) {
  jglockfree::SpscQueue<std::string, 4> queue;

  EXPECT_TRUE(queue.TryEnqueue("hello"));
  EXPECT_TRUE(queue.TryEnqueue("world"));

  EXPECT_EQ(queue.TryDequeue(), "hello");
  EXPECT_EQ(queue.TryDequeue(), "world");
}

// ============================================================================
// Concurrent Producer-Consumer Tests
// ============================================================================

TEST(SpscQueueTest, SingleProducerSingleConsumer) {
  // The core SPSC test: one thread produces, one thread consumes
  // All items should arrive in order with no corruption

  constexpr std::size_t kNumItems = 100000;
  jglockfree::SpscQueue<std::size_t, 1024> queue;

  std::thread producer([&queue, kNumItems]() {
    std::ranges::for_each(std::ranges::views::iota(std::size_t{0}, kNumItems),
                          [&queue](std::size_t i) { SpinEnqueue(queue, i); });
  });

  std::thread consumer([&queue]() {
    std::size_t expected = 0;
    while (expected < kNumItems) {
      const auto result = queue.TryDequeue();
      if (result.has_value()) {
        EXPECT_EQ(*result, expected)
            << "Out of order or corrupted at index " << expected;
        ++expected;
      }
      // If nullopt, queue was momentarily empty - spin and retry
    }
  });

  producer.join();
  consumer.join();
}

TEST(SpscQueueTest, ProducerFasterThanConsumer) {
  // Producer bursts items faster than consumer drains them
  // Tests backpressure behavior when queue fills up

  constexpr std::size_t kNumItems = 10000;
  jglockfree::SpscQueue<std::size_t, 64>
      queue;  // Small buffer forces backpressure

  std::thread producer([&queue, kNumItems]() {
    std::ranges::for_each(std::ranges::views::iota(std::size_t{0}, kNumItems),
                          [&queue](std::size_t i) {
                            SpinEnqueue(queue, i);
                            // no delay, the producer goes as fast as possible
                          });
  });

  std::thread consumer([&]() {
    std::size_t expected = 0;

    while (expected < kNumItems) {
      const auto result = queue.TryDequeue();
      if (result.has_value()) {
        EXPECT_EQ(*result, expected++);
        // Artificial slowdown
        std::this_thread::sleep_for(std::chrono::microseconds(1));
      }
    }
  });

  producer.join();
  consumer.join();
}

TEST(SpscQueueTest, ConsumerFasterThanProducer) {
  // Consumer spins waiting for items; producer is slow
  // Tests empty-queue polling behavior

  constexpr std::size_t kNumItems = 1000;
  jglockfree::SpscQueue<std::size_t, 64> queue;

  std::thread producer([&queue, kNumItems]() {
    std::ranges::for_each(
        std::ranges::views::iota(std::size_t{0}, kNumItems), [&queue](std::size_t i) {
          std::this_thread::sleep_for(std::chrono::microseconds(10));
          SpinEnqueue(queue, i);
        });
  });

  std::thread consumer([&queue]() {
    std::size_t expected = 0;
    while (expected < kNumItems) {
      const auto result = queue.TryDequeue();
      if (result.has_value()) {
        EXPECT_EQ(*result, expected++);
      }
    }
  });

  producer.join();
  consumer.join();
}

TEST(SpscQueueTest, StressTestLongRunning) {
  // Extended test to catch subtle race conditions that only
  // manifest under sustained load

  constexpr std::size_t kNumItems = 1000000;
  jglockfree::SpscQueue<std::size_t, 512> queue;

  std::atomic<std::size_t> checksum_produced{0};
  std::atomic<std::size_t> checksum_consumed{0};

  std::thread producer([&queue, kNumItems, &checksum_produced]() {
    std::ranges::for_each(std::ranges::views::iota(std::size_t{0}, kNumItems),
                          [&queue, &checksum_produced](std::size_t i) {
                            SpinEnqueue(queue, i);
                            checksum_produced.fetch_add(
                                i, std::memory_order_relaxed);
                          });
  });

  std::thread consumer([&]() {
    std::size_t count = 0;
    while (count < kNumItems) {
      const auto result = queue.TryDequeue();
      if (result.has_value()) {
        checksum_consumed.fetch_add(*result, std::memory_order_relaxed);
        ++count;
      }
    }
  });

  producer.join();
  consumer.join();

  // If any items were lost or corrupted, checksums won't match
  EXPECT_EQ(checksum_produced.load(), checksum_consumed.load());
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST(SpscQueueTest, MinimalBufferSize) {
  // Buffer of NumSlots=1 means array size 2, capacity of 1
  jglockfree::SpscQueue<int, 1> queue;

  EXPECT_TRUE(queue.TryEnqueue(42));
  EXPECT_FALSE(queue.TryEnqueue(43)) << "Should be full with capacity 1";

  EXPECT_EQ(queue.TryDequeue(), 42);
  EXPECT_EQ(queue.TryDequeue(), std::nullopt);

  EXPECT_TRUE(queue.TryEnqueue(44));
  EXPECT_EQ(queue.TryDequeue(), 44);
}

TEST(SpscQueueTest, RepeatedFillAndDrain) {
  // Completely fill, completely drain, repeat many times
  jglockfree::SpscQueue<int, 8> queue;

  std::ranges::for_each(std::ranges::views::iota(0, 100), [&queue](int round) {
    std::ranges::for_each(std::ranges::views::iota(0, 8),
                          [&queue, round](int i) {
                            EXPECT_TRUE(queue.TryEnqueue(round * 100 + i));
                          });
    EXPECT_FALSE(queue.TryEnqueue(-1)) << "Should be full at round " << round;

    std::ranges::for_each(std::ranges::views::iota(0, 8),
                          [&queue, round](int i) {
                            auto result = queue.TryDequeue();
                            ASSERT_TRUE(result.has_value());
                            EXPECT_EQ(*result, round * 100 + i);
                          });
    EXPECT_EQ(queue.TryDequeue(), std::nullopt);
  });
}

TEST(SpscQueueTest, TryEnqueueReturnsFalseWhenFull) {
  jglockfree::SpscQueue<int, 4> queue;  // Capacity of 4

  EXPECT_TRUE(queue.TryEnqueue(1));
  EXPECT_TRUE(queue.TryEnqueue(2));
  EXPECT_TRUE(queue.TryEnqueue(3));
  EXPECT_TRUE(queue.TryEnqueue(4));
  EXPECT_FALSE(queue.TryEnqueue(5)) << "Fifth enqueue should fail";

  // Drain one and try again
  EXPECT_EQ(queue.TryDequeue(), 1);
  EXPECT_TRUE(queue.TryEnqueue(5)) << "Should succeed after drain";
}

using namespace std::chrono_literals;

// ============================================================================
// Basic Blocking Operations
// ============================================================================

TEST(SpscQueueBlockingTest, BlockingDequeueWaitsForItem) {
  jglockfree::SpscQueue<int, 4> queue;
  std::atomic<bool> consumer_started{false};
  std::atomic<bool> consumer_done{false};
  int received = 0;

  std::thread consumer([&] {
    consumer_started.store(true, std::memory_order_release);
    received = queue.Dequeue();  // Should block here
    consumer_done.store(true, std::memory_order_release);
  });

  // Wait for consumer to start and block
  while (!consumer_started.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  std::this_thread::sleep_for(10ms);  // Give consumer time to actually block

  // Consumer should still be waiting
  EXPECT_FALSE(consumer_done.load(std::memory_order_acquire));

  // Now enqueue something
  queue.Enqueue(42);

  consumer.join();

  EXPECT_TRUE(consumer_done.load(std::memory_order_acquire));
  EXPECT_EQ(received, 42);
}

TEST(SpscQueueBlockingTest, BlockingEnqueueWaitsForSpace) {
  jglockfree::SpscQueue<int, 2> queue;  // Capacity of 2
  std::atomic<bool> producer_started{false};
  std::atomic<bool> producer_done{false};

  // Fill the queue
  EXPECT_TRUE(queue.TryEnqueue(1));
  EXPECT_TRUE(queue.TryEnqueue(2));
  EXPECT_FALSE(queue.TryEnqueue(3));  // Should fail, queue full

  std::thread producer([&] {
    producer_started.store(true, std::memory_order_release);
    queue.Enqueue(3);  // Should block here
    producer_done.store(true, std::memory_order_release);
  });

  // Wait for producer to start and block
  while (!producer_started.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  std::this_thread::sleep_for(10ms);  // Give producer time to actually block

  // Producer should still be waiting
  EXPECT_FALSE(producer_done.load(std::memory_order_acquire));

  // Dequeue one item to make space
  EXPECT_EQ(queue.TryDequeue(), 1);

  producer.join();

  EXPECT_TRUE(producer_done.load(std::memory_order_acquire));

  // Verify all items are there in order
  EXPECT_EQ(queue.TryDequeue(), 2);
  EXPECT_EQ(queue.TryDequeue(), 3);
  EXPECT_EQ(queue.TryDequeue(), std::nullopt);
}

TEST(SpscQueueBlockingTest, BlockingDequeueReturnsImmediatelyWhenNotEmpty) {
  jglockfree::SpscQueue<int, 4> queue;

  queue.Enqueue(42);

  auto start = std::chrono::steady_clock::now();
  int result = queue.Dequeue();
  auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_EQ(result, 42);
  EXPECT_LT(elapsed, 10ms);  // Should be nearly instant
}

TEST(SpscQueueBlockingTest, BlockingEnqueueReturnsImmediatelyWhenNotFull) {
  jglockfree::SpscQueue<int, 4> queue;

  auto start = std::chrono::steady_clock::now();
  queue.Enqueue(42);
  auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_LT(elapsed, 10ms);  // Should be nearly instant
  EXPECT_EQ(queue.TryDequeue(), 42);
}

// ============================================================================
// Producer-Consumer with Blocking
// ============================================================================

TEST(SpscQueueBlockingTest, ProducerConsumerWithBlocking) {
  constexpr std::size_t kNumItems = 10000;
  jglockfree::SpscQueue<std::size_t, 64> queue;

  std::thread producer([&] {
    std::ranges::for_each(
        std::ranges::views::iota(std::size_t{0}, kNumItems),
        [&queue](std::size_t i) { queue.Enqueue(std::move(i)); });
  });

  std::thread consumer([&] {
    std::ranges::for_each(std::ranges::views::iota(std::size_t{0}, kNumItems),
                          [&queue](std::size_t expected) {
                            std::size_t received = queue.Dequeue();
                            EXPECT_EQ(received, expected);
                          });
  });

  producer.join();
  consumer.join();
}

TEST(SpscQueueBlockingTest, SlowProducerFastConsumer) {
  constexpr std::size_t kNumItems = 100;
  jglockfree::SpscQueue<std::size_t, 8> queue;

  std::thread producer([&] {
    std::ranges::for_each(std::ranges::views::iota(std::size_t{0}, kNumItems),
                          [&queue](std::size_t i) {
                            std::this_thread::sleep_for(100us);
                            queue.Enqueue(std::move(i));
                          });
  });

  std::thread consumer([&] {
    std::ranges::for_each(
        std::ranges::views::iota(std::size_t{0}, kNumItems),
        [&queue](std::size_t expected) {
          std::size_t received =
              queue.Dequeue();  // Will block waiting for slow producer
          EXPECT_EQ(received, expected);
        });
  });

  producer.join();
  consumer.join();
}

TEST(SpscQueueBlockingTest, FastProducerSlowConsumer) {
  constexpr std::size_t kNumItems = 100;
  jglockfree::SpscQueue<std::size_t, 8>
      queue;  // Small buffer forces producer to block

  std::thread producer([&] {
    std::ranges::for_each(
        std::ranges::views::iota(std::size_t{0}, kNumItems),
        [&queue](std::size_t i) {
          queue.Enqueue(std::move(i));  // Will block when buffer fills
        });
  });

  std::thread consumer([&] {
    std::ranges::for_each(std::ranges::views::iota(std::size_t{0}, kNumItems),
                          [&queue](std::size_t expected) {
                            std::this_thread::sleep_for(100us);
                            std::size_t received = queue.Dequeue();
                            EXPECT_EQ(received, expected);
                          });
  });

  producer.join();
  consumer.join();
}

// ============================================================================
// Mixed Blocking and Non-Blocking
// ============================================================================

TEST(SpscQueueBlockingTest, MixedBlockingAndTryOperations) {
  jglockfree::SpscQueue<int, 4> queue;

  // Use blocking enqueue
  queue.Enqueue(1);
  queue.Enqueue(2);

  // Use try dequeue
  EXPECT_EQ(queue.TryDequeue(), 1);

  // Use blocking dequeue
  EXPECT_EQ(queue.Dequeue(), 2);

  // Use try enqueue
  EXPECT_TRUE(queue.TryEnqueue(3));

  // Use blocking dequeue
  EXPECT_EQ(queue.Dequeue(), 3);
}

// ============================================================================
// Stress Test
// ============================================================================

TEST(SpscQueueBlockingTest, StressTestBlocking) {
  constexpr std::size_t kNumItems = 100000;
  jglockfree::SpscQueue<std::size_t, 128> queue;

  std::atomic<std::size_t> checksum_produced{0};
  std::atomic<std::size_t> checksum_consumed{0};

  std::thread producer([&] {
    auto items = std::ranges::views::iota(std::size_t{0}, kNumItems);
    std::ranges::for_each(items, [&](std::size_t item) {
      queue.Enqueue(std::move(item));
      checksum_produced.fetch_add(item, std::memory_order_relaxed);
    });
  });

  std::thread consumer([&] {
    std::ranges::for_each(
        std::ranges::views::iota(std::size_t{0}, kNumItems), [&](std::size_t) {
          std::size_t val = queue.Dequeue();
          checksum_consumed.fetch_add(val, std::memory_order_relaxed);
        });
  });

  producer.join();
  consumer.join();

  EXPECT_EQ(checksum_produced.load(), checksum_consumed.load());
}

// ============================================================================
// Move-Only Types with Blocking
// ============================================================================

TEST(SpscQueueBlockingTest, BlockingWithMoveOnlyTypes) {
  jglockfree::SpscQueue<std::unique_ptr<int>, 4> queue;

  std::thread producer([&] {
    std::ranges::for_each(std::ranges::views::iota(0, 10), [&queue](int i) {
      queue.Enqueue(std::make_unique<int>(i));
    });
  });

  std::thread consumer([&] {
    std::ranges::for_each(std::ranges::views::iota(0, 10),
                          [&queue](const int expected) {
                            auto ptr = queue.Dequeue();
                            EXPECT_EQ(*ptr, expected);
                          });
  });

  producer.join();
  consumer.join();
}
