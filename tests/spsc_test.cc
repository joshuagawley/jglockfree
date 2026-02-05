// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>
#include <jglockfree/spsc.h>

#include <chrono>
#include <thread>
#include <vector>

struct ThrowsOnMove {
  int value;
  static inline int move_count = 0;
  static inline int throw_after = std::numeric_limits<int>::max();

  constexpr ThrowsOnMove() : value(-1) {}
  constexpr explicit ThrowsOnMove(int v) : value(v) {}

  ThrowsOnMove(const ThrowsOnMove &other) : value(other.value) {}
  ThrowsOnMove &operator=(const ThrowsOnMove &) = default;

  ThrowsOnMove(ThrowsOnMove &&other) : value(other.value) {
    if (++move_count >= throw_after) {
      throw std::runtime_error("move failed");
    }
  }

  ThrowsOnMove &operator=(ThrowsOnMove &&other) {
    if (++move_count >= throw_after) {
      throw std::runtime_error("Move assignment failed");
    }
    value = other.value;
    return *this;
  }

  static void Reset(int throw_at);
};

void ThrowsOnMove::Reset(int throw_at = std::numeric_limits<int>::max()) {
  move_count = 0;
  throw_after = throw_at;
}

// Helper: spin-enqueue until successful
template <typename T, int N>
void SpinEnqueue(jglockfree::SpscQueue<T, N> &queue, T value) {
  while (not queue.TryEnqueue(std::move(value))) {
    std::this_thread::yield();
  }
}

// Helper: spin-dequeue until successful
template <typename T, int N>
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

  for (int i = 0; i < 8; ++i) {
    EXPECT_TRUE(queue.TryEnqueue(std::move(i)))
        << "Failed to enqueue element " << i;
  }

  // Queue should now be full
  EXPECT_FALSE(queue.TryEnqueue(999)) << "Queue should be full";

  // All 8 should come back out
  for (int i = 0; i < 8; ++i) {
    const auto result = queue.TryDequeue();
    ASSERT_TRUE(result.has_value()) << "Failed to dequeue element " << i;
    EXPECT_EQ(*result, i);
  }

  EXPECT_EQ(queue.TryDequeue(), std::nullopt);
}

TEST(SpscQueueTest, WrapAroundBehavior) {
  // Force the indices to wrap around the array boundary
  jglockfree::SpscQueue<int, 4> queue;  // Capacity of 4, array size 5

  // Fill and drain a few times to move head/tail past the array end
  for (int round = 0; round < 5; ++round) {
    EXPECT_TRUE(queue.TryEnqueue(round * 10 + 1));
    EXPECT_TRUE(queue.TryEnqueue(round * 10 + 2));
    EXPECT_TRUE(queue.TryEnqueue(round * 10 + 3));

    EXPECT_EQ(queue.TryDequeue(), round * 10 + 1);
    EXPECT_EQ(queue.TryDequeue(), round * 10 + 2);
    EXPECT_EQ(queue.TryDequeue(), round * 10 + 3);
  }
}

TEST(SpscQueueTest, PartialFillDrainWrapAround) {
  jglockfree::SpscQueue<int, 4> queue;  // Capacity of 4

  // Enqueue one, dequeue one - keeps queue at 0-1 items while indices wrap
  for (int i = 0; i < 5; ++i) {
    EXPECT_TRUE(queue.TryEnqueue(std::move(i)))
        << "Failed to enqueue at i=" << i;
    const auto result = queue.TryDequeue();
    ASSERT_TRUE(result.has_value()) << "Failed to dequeue at i=" << i;
    EXPECT_EQ(*result, i);
  }
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

  constexpr int kNumItems = 100000;
  jglockfree::SpscQueue<int, 1024> queue;

  std::thread producer([&queue]() {
    for (int i = 0; i < kNumItems; ++i) {
      SpinEnqueue(queue, i);
    }
  });

  std::thread consumer([&queue]() {
    int expected = 0;
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

  std::thread producer([&queue]() {
    for (std::size_t i = 0; i < kNumItems; ++i) {
      SpinEnqueue(queue, i);
      // no delay, the producer goes as fast as possible
    }
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

  std::thread producer([&queue]() {
    for (std::size_t i = 0; i < kNumItems; ++i) {
      std::this_thread::sleep_for(std::chrono::microseconds(10));
      SpinEnqueue(queue, i);
    }
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

  std::thread producer([&queue, &checksum_produced]() {
    for (std::size_t i = 0; i < kNumItems; ++i) {
      SpinEnqueue(queue, i);
      checksum_produced.fetch_add(i, std::memory_order_relaxed);
    }
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

  for (int round = 0; round < 100; ++round) {
    for (int i = 0; i < 8; ++i) {
      EXPECT_TRUE(queue.TryEnqueue(round * 100 + i));
    }
    EXPECT_FALSE(queue.TryEnqueue(-1)) << "Should be full at round " << round;

    for (int i = 0; i < 8; ++i) {
      auto result = queue.TryDequeue();
      ASSERT_TRUE(result.has_value());
      EXPECT_EQ(*result, round * 100 + i);
    }
    EXPECT_EQ(queue.TryDequeue(), std::nullopt);
  }
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
  while (not consumer_started.load(std::memory_order_acquire)) {
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
  while (not producer_started.load(std::memory_order_acquire)) {
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

TEST(SpscQueueBlockingTest, EnqueueDoesNotConsumeValueOnFullQueue) {
  // Regression test: Enqueue must not move-from value until space is available.
  // Previously, Enqueue called std::move(value) on every spin iteration,
  // corrupting move-only types when the queue was full.

  jglockfree::SpscQueue<std::unique_ptr<int>, 2> queue;  // capacity of 2

  // Fill the queue
  ASSERT_TRUE(queue.TryEnqueue(std::make_unique<int>(1)));
  ASSERT_TRUE(queue.TryEnqueue(std::make_unique<int>(2)));
  ASSERT_FALSE(queue.TryEnqueue(std::make_unique<int>(3)));  // full

  std::atomic<bool> enqueue_started{false};
  std::atomic<bool> enqueue_done{false};

  // Producer: will block because queue is full
  std::thread producer([&] {
    auto ptr = std::make_unique<int>(42);
    enqueue_started.store(true, std::memory_order_release);
    queue.Enqueue(std::move(ptr));  // blocks until consumer makes space
    enqueue_done.store(true, std::memory_order_release);
  });

  // Wait for producer to start spinning/blocking
  while (not enqueue_started.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  // Producer should still be blocked
  EXPECT_FALSE(enqueue_done.load(std::memory_order_acquire));

  // Consumer: drain one item to make space
  auto first = queue.Dequeue();
  EXPECT_EQ(*first, 1);

  // Wait for producer to complete
  producer.join();
  EXPECT_TRUE(enqueue_done.load(std::memory_order_acquire));

  // Drain remaining items and verify the blocked value arrived intact
  auto second = queue.Dequeue();
  EXPECT_EQ(*second, 2);

  auto third = queue.Dequeue();
  ASSERT_NE(third, nullptr) << "Value was corrupted by premature move";
  EXPECT_EQ(*third, 42) << "Value was corrupted by premature move";
}

// ============================================================================
// Producer-Consumer with Blocking
// ============================================================================

TEST(SpscQueueBlockingTest, ProducerConsumerWithBlocking) {
  constexpr std::size_t kNumItems = 10000;
  jglockfree::SpscQueue<std::size_t, 64> queue;

  std::thread producer([&] {
    for (std::size_t i = 0; i < kNumItems; ++i) {
      queue.Enqueue(std::move(i));
    }
  });

  std::thread consumer([&] {
    for (std::size_t expected = 0; expected < kNumItems; ++expected) {
      std::size_t received = queue.Dequeue();
      EXPECT_EQ(received, expected);
    }
  });

  producer.join();
  consumer.join();
}

TEST(SpscQueueBlockingTest, SlowProducerFastConsumer) {
  constexpr std::size_t kNumItems = 100;
  jglockfree::SpscQueue<std::size_t, 8> queue;

  std::thread producer([&] {
    for (std::size_t i = 0; i < kNumItems; ++i) {
      std::this_thread::sleep_for(100us);
      queue.Enqueue(std::move(i));
    }
  });

  std::thread consumer([&] {
    for (std::size_t expected = 0; expected < kNumItems; ++expected) {
      std::size_t received =
          queue.Dequeue();  // Will block waiting for slow producer
      EXPECT_EQ(received, expected);
    }
  });

  producer.join();
  consumer.join();
}

TEST(SpscQueueBlockingTest, FastProducerSlowConsumer) {
  constexpr std::size_t kNumItems = 100;
  jglockfree::SpscQueue<std::size_t, 8>
      queue;  // Small buffer forces producer to block

  std::thread producer([&] {
    for (std::size_t i = 0; i < kNumItems; ++i) {
      queue.Enqueue(std::move(i));  // Will block when buffer fills
    }
  });

  std::thread consumer([&] {
    for (std::size_t expected = 0; expected < kNumItems; ++expected) {
      std::this_thread::sleep_for(100us);
      std::size_t received = queue.Dequeue();
      EXPECT_EQ(received, expected);
    }
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
    for (std::size_t item = 0; item < kNumItems; ++item) {
      queue.Enqueue(std::move(item));
      checksum_produced.fetch_add(item, std::memory_order_relaxed);
    }
  });

  std::thread consumer([&] {
    for (std::size_t i = 0; i < kNumItems; ++i) {
      std::size_t val = queue.Dequeue();
      checksum_consumed.fetch_add(val, std::memory_order_relaxed);
    }
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
    for (std::size_t i = 0; i < 10; ++i) {
      queue.Enqueue(std::make_unique<int>(i));
    }
  });

  std::thread consumer([&] {
    for (std::size_t expected = 0; expected < 10; ++expected) {
      auto ptr = queue.Dequeue();
      EXPECT_EQ(*ptr, expected);
    }
  });

  producer.join();
  consumer.join();
}

// ============================================================================
// Exception Safety Tests
// ============================================================================

TEST(SpscQueueExceptionTest, EnqueueExceptionLeavesQueueUsable) {
  jglockfree::SpscQueue<ThrowsOnMove, 4> queue;

  // Successfully enqueue one item
  ThrowsOnMove::Reset();
  ASSERT_TRUE(queue.TryEnqueue(ThrowsOnMove{1}));

  // Set up to throw on next move
  ThrowsOnMove::Reset(1);
  EXPECT_THROW((void)queue.TryEnqueue(ThrowsOnMove{2}), std::runtime_error);

  // Queue should still be usable
  ThrowsOnMove::Reset();
  ASSERT_TRUE(queue.TryEnqueue(ThrowsOnMove{3}));

  // Verify we can dequeue both items
  auto first = queue.TryDequeue();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->value, 1);

  auto second = queue.TryDequeue();
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->value, 3);

  EXPECT_FALSE(queue.TryDequeue().has_value());
}

TEST(SpscQueueExceptionTest, DequeueExceptionLeavesQueueUsable) {
  jglockfree::SpscQueue<ThrowsOnMove, 4> queue;

  // Enqueue some items
  ThrowsOnMove::Reset();
  ASSERT_TRUE(queue.TryEnqueue(ThrowsOnMove{1}));
  ASSERT_TRUE(queue.TryEnqueue(ThrowsOnMove{2}));

  // Set up to throw on dequeue's move
  ThrowsOnMove::Reset(1);
  EXPECT_THROW((void)queue.TryDequeue(), std::runtime_error);

  // Queue should still be usable
  // Note: The item that threw is NOT consumed—head didn't advance.
  // This means we can retry and get the same item.
  ThrowsOnMove::Reset();

  // First item is still there (the one that threw)
  auto first = queue.TryDequeue();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->value, 1);

  // Second item
  auto second = queue.TryDequeue();
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->value, 2);

  EXPECT_FALSE(queue.TryDequeue().has_value());
}

// ============================================================================
// Non-Power-of-Two Size Tests
// ============================================================================

TEST(SpscQueueTest, NonPowerOfTwoSize_Three) {
  jglockfree::SpscQueue<int, 3> queue;  // capacity of 3

  EXPECT_TRUE(queue.TryEnqueue(1));
  EXPECT_TRUE(queue.TryEnqueue(2));
  EXPECT_TRUE(queue.TryEnqueue(3));
  EXPECT_FALSE(queue.TryEnqueue(4));  // full

  EXPECT_EQ(queue.TryDequeue(), 1);
  EXPECT_EQ(queue.TryDequeue(), 2);
  EXPECT_EQ(queue.TryDequeue(), 3);
  EXPECT_EQ(queue.TryDequeue(), std::nullopt);
}

TEST(SpscQueueTest, NonPowerOfTwoSize_Seven) {
  jglockfree::SpscQueue<int, 7> queue;  // capacity of 7

  // Fill completely
  for (int i = 0; i < 7; ++i) {
    EXPECT_TRUE(queue.TryEnqueue(i)) << "Failed at i=" << i;
  }
  EXPECT_FALSE(queue.TryEnqueue(99));  // full

  // Drain completely
  for (int i = 0; i < 7; ++i) {
    auto result = queue.TryDequeue();
    ASSERT_TRUE(result.has_value()) << "Failed at i=" << i;
    EXPECT_EQ(*result, i);
  }
  EXPECT_EQ(queue.TryDequeue(), std::nullopt);
}

TEST(SpscQueueTest, NonPowerOfTwoSize_WrapAround) {
  // Test that modulo arithmetic works correctly at boundaries
  jglockfree::SpscQueue<int, 5> queue;  // array size 6, capacity 5

  // Do many cycles to exercise wraparound
  for (int cycle = 0; cycle < 20; ++cycle) {
    // Partially fill
    for (int i = 0; i < 3; ++i) {
      EXPECT_TRUE(queue.TryEnqueue(cycle * 100 + i));
    }

    // Partially drain
    for (int i = 0; i < 3; ++i) {
      auto result = queue.TryDequeue();
      ASSERT_TRUE(result.has_value());
      EXPECT_EQ(*result, cycle * 100 + i);
    }
  }
}

TEST(SpscQueueTest, NonPowerOfTwoSize_ConcurrentWrapAround) {
  constexpr std::size_t kNumItems = 100000;
  jglockfree::SpscQueue<std::size_t, 7> queue;  // odd size, small buffer

  std::thread producer([&] {
    for (std::size_t i = 0; i < kNumItems; ++i) {
      while (not queue.TryEnqueue(std::move(i))) {
        std::this_thread::yield();
      }
    }
  });

  std::thread consumer([&] {
    for (std::size_t expected = 0; expected < kNumItems; ++expected) {
      while (true) {
        auto result = queue.TryDequeue();
        if (result.has_value()) {
          EXPECT_EQ(*result, expected);
          break;
        }
        std::this_thread::yield();
      }
    }
  });

  producer.join();
  consumer.join();
}

TEST(SpscQueueTest, DestructorWithRemainingItems) {
  auto witness = std::make_shared<int>(42);

  {
    jglockfree::SpscQueue<std::shared_ptr<int>, 8> queue;
    ASSERT_TRUE(queue.TryEnqueue(witness));
    ASSERT_TRUE(queue.TryEnqueue(witness));
    ASSERT_TRUE(queue.TryEnqueue(witness));

    EXPECT_EQ(witness.use_count(), 4);
  }

  // Note: SPSC queue with trivial destructor may not clean up items in slots!
  // This test documents current behaviour - if it fails, you've added cleanup
  // If use_count is still 4 here, the queue isn't destroying remaining items
  // If use_count is 1, the queue properly destroys them

  // TODO: Document expected behaviour - should SPSC clean up remaining items?
  // For now, just ensure no crash
}

TEST(SpscQueueTest, DestructorWhenEmpty) {
  {
    jglockfree::SpscQueue<int, 8> queue;
    // No enqueue, just destroy
  }
  // Should not crash
}

TEST(SpscQueueTest, DestructorAfterDrain) {
  auto witness = std::make_shared<int>(42);

  {
    jglockfree::SpscQueue<std::shared_ptr<int>, 8> queue;
    ASSERT_TRUE(queue.TryEnqueue(witness));
    ASSERT_TRUE(queue.TryEnqueue(witness));

    EXPECT_EQ(witness.use_count(), 3);

    (void)queue.TryDequeue();
    (void)queue.TryDequeue();

    EXPECT_EQ(witness.use_count(), 1);
  }

  EXPECT_EQ(witness.use_count(), 1);
}
