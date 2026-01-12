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
