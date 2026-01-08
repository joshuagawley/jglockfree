// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>
#include <jglockfree/queue.h>

#include <ranges>
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

  std::ranges::for_each(
      std::ranges::views::iota(0, kNumProducers), [&](const int p) {
        producers.emplace_back([&] {
          std::ranges::for_each(
              std::ranges::views::iota(0, kItemsPerProducer),
              [&](const int i) { queue.Enqueue(p * kItemsPerProducer + i); });
        });
      });

  std::ranges::for_each(producers, [](auto &t) { t.join(); });

  std::ranges::for_each(std::ranges::views::iota(0, kNumConsumers), [&](int _) {
    consumers.emplace_back([&queue, &total_dequeued] {
      while (auto _ = queue.Dequeue()) {
        total_dequeued.fetch_add(1, std::memory_order_relaxed);
      }
    });
  });

  std::ranges::for_each(consumers, [](auto &t) { t.join(); });

  EXPECT_EQ(total_dequeued.load(), kNumProducers * kItemsPerProducer);
}
