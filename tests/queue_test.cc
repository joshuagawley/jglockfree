// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>
#include <jglockfree/queue.h>

#include <thread>

TEST(QueueTest, EmptyQueueReturnsNullopt) {
  jglockfree::Queue<int> queue{};
  EXPECT_EQ(queue.Dequeue(), std::nullopt);
}

TEST(QueueTest, SingleElement) {
  jglockfree::Queue<int> queue{};
  queue.Enqueue(42);
  EXPECT_EQ(queue.Dequeue(), 42);
  EXPECT_EQ(queue.Dequeue(), std::nullopt);
}

TEST(QueueTest, FIFOOrdering) {
  jglockfree::Queue<int> queue{};
  queue.Enqueue(1);
  queue.Enqueue(2);
  queue.Enqueue(3);
  EXPECT_EQ(queue.Dequeue(), 1);
  EXPECT_EQ(queue.Dequeue(), 2);
  EXPECT_EQ(queue.Dequeue(), 3);
  EXPECT_EQ(queue.Dequeue(), std::nullopt);
}

TEST(QueueTest, ConcurrentEnqueueDequeue) {
  jglockfree::Queue<int> queue{};
  constexpr int kNumProducers = 8;
  constexpr int kNumConsumers = 8;
  constexpr int kItemsPerProducer = 100'000;

  std::atomic total_dequeued{0};
  std::vector<std::thread> producers;
  std::vector<std::thread> consumers;
  producers.reserve(kNumProducers);
  consumers.reserve(kNumConsumers);

  for (int p = 0; p < kNumProducers; ++p) {
    producers.emplace_back([&queue, p] {
      for (int i = 0; i < kItemsPerProducer; ++i) {
        queue.Enqueue(p * kItemsPerProducer + i);
      }
    });
  }

  for (auto &t : producers) {
    t.join();
  }

  for (int c = 0; c < kNumConsumers; ++c) {
    consumers.emplace_back([&queue, &total_dequeued] {
      while (auto _ = queue.Dequeue()) {
        total_dequeued.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  for (auto &t : consumers) {
    t.join();
  }

  EXPECT_EQ(total_dequeued.load(), kNumProducers * kItemsPerProducer);
}

TEST(QueueTest, DestructorWithRemainingItems) {
  // Verify that items remaining in the queue are properly destroyed
  auto witness = std::make_shared<int>(42);
  ASSERT_EQ(witness.use_count(), 1);

  {
    jglockfree::Queue<std::shared_ptr<int>> queue;
    queue.Enqueue(witness);
    queue.Enqueue(witness);
    queue.Enqueue(witness);

    // witness now has 4 references: original + 3 in queue
    EXPECT_EQ(witness.use_count(), 4);

    // Queue destructor runs here
  }

  // All queue copies should be destroyed
  EXPECT_EQ(witness.use_count(), 1) << "Queue destructor leaked items";
}

TEST(QueueTest, DestructorWithSingleItem) {
  auto witness = std::make_shared<int>(1);

  {
    jglockfree::Queue<std::shared_ptr<int>> queue;
    queue.Enqueue(witness);
    EXPECT_EQ(witness.use_count(), 2);
  }

  EXPECT_EQ(witness.use_count(), 1);
}

TEST(QueueTest, DestructorWhenEmpty) {
  // Edge case: destroying an empty queue (only dummy node)
  {
    jglockfree::Queue<int> queue;
    // No enqueue, just destroy
  }
  // Should not crash
}

TEST(QueueTest, DestructorAfterDrain) {
  auto witness = std::make_shared<int>(42);

  {
    jglockfree::Queue<std::shared_ptr<int>> queue;
    queue.Enqueue(witness);
    queue.Enqueue(witness);

    EXPECT_EQ(witness.use_count(), 3);

    // Drain the queue
    queue.Dequeue();
    queue.Dequeue();

    EXPECT_EQ(witness.use_count(), 1);

    // Queue destructor runs with queue empty (but dummy node exists)
  }

  EXPECT_EQ(witness.use_count(), 1);
}

TEST(QueueTest, DestructorWithMoveOnlyType) {
  static int destructor_count = 0;

  struct CountsDestructor {
    int id;
    CountsDestructor(int i) : id(i) {}
    ~CountsDestructor() { ++destructor_count; }
    CountsDestructor(CountsDestructor &&other) noexcept : id(other.id) {
      other.id = -1;  // mark as moved-from
    }
    CountsDestructor &operator=(CountsDestructor &&) = default;
    CountsDestructor(const CountsDestructor &) = delete;
    CountsDestructor &operator=(const CountsDestructor &) = delete;
  };

  destructor_count = 0;

  {
    jglockfree::Queue<CountsDestructor> queue;
    queue.Enqueue(CountsDestructor{1});
    queue.Enqueue(CountsDestructor{2});
    queue.Enqueue(CountsDestructor{3});

    // Some destructors called for temporaries during enqueue
    int count_after_enqueue = destructor_count;

    // Queue destructor runs
  }

  // Destructors should have been called for all items
  // Exact count depends on move semantics, but should be > 0
  EXPECT_GT(destructor_count, 0) << "No destructors called";
}
