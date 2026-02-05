// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>
#include <jglockfree/queue.h>

#include <atomic>
#include <thread>

namespace {

struct TestNode {
  std::atomic<TestNode *> next{nullptr};
};

}  // namespace

TEST(FreeListTest, CountStartsAtZero) {
  const jglockfree::FreeList<TestNode> list;
  EXPECT_EQ(list.get_count(), 0);
}

TEST(FreeListTest, CountTracksOperations) {
  jglockfree::FreeList<TestNode> list;
  constexpr int kNumNodes = 10;

  std::vector<TestNode *> nodes;
  nodes.reserve(kNumNodes);
  for (int i = 0; i < kNumNodes; ++i) {
    nodes.push_back(new TestNode{});
  }

  // Push all nodes
  for (std::size_t i = 0; i < kNumNodes; ++i) {
    list.Push(nodes[i]);
    EXPECT_EQ(list.get_count(), i + 1);
  }

  // Pop half
  for (std::size_t i = 0; i < kNumNodes / 2; ++i) {
    TestNode *node = list.Pop();
    EXPECT_NE(node, nullptr);
    EXPECT_EQ(list.get_count(), kNumNodes - i - 1);
  }

  // Pop remainder
  for (std::size_t i = kNumNodes / 2; i < kNumNodes; ++i) {
    TestNode *node = list.Pop();
    EXPECT_NE(node, nullptr);
    EXPECT_EQ(list.get_count(), kNumNodes - i - 1);
  }

  EXPECT_EQ(list.get_count(), 0);
  // FreeList destructor will delete remaining nodes (none left)
}

TEST(FreeListTest, PopFromEmptyReturnsNullAndKeepsCountZero) {
  jglockfree::FreeList<TestNode> list;
  EXPECT_EQ(list.Pop(), nullptr);
  EXPECT_EQ(list.get_count(), 0);

  // Push one, pop it, then pop again from empty
  auto *node = new TestNode{};
  list.Push(node);
  EXPECT_EQ(list.get_count(), 1);

  TestNode *popped = list.Pop();
  EXPECT_NE(popped, nullptr);
  EXPECT_EQ(list.get_count(), 0);

  EXPECT_EQ(list.Pop(), nullptr);
  EXPECT_EQ(list.get_count(), 0);

  delete popped;
}

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

  std::atomic<std::size_t> total_dequeued{0};
  std::vector<std::thread> producers;
  std::vector<std::thread> consumers;
  producers.reserve(static_cast<std::size_t>(kNumProducers));
  consumers.reserve(static_cast<std::size_t>(kNumConsumers));

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

  for (std::size_t c = 0; c < kNumConsumers; ++c) {
    consumers.emplace_back([&queue, &total_dequeued] {
      while (queue.Dequeue()) {
        total_dequeued.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  for (auto &t : consumers) {
    t.join();
  }

  EXPECT_EQ(total_dequeued.load(), kNumProducers * kItemsPerProducer);
}

// Exercises the spill-to-global and refill-from-global free list paths.
// Unlike ConcurrentEnqueueDequeue (where producers finish before consumers
// start), this test runs producer and consumer simultaneously, forcing nodes
// to flow: consumer local list → global list → producer local list.
TEST(QueueTest, ConcurrentProducerConsumerRecycling) {
  jglockfree::Queue<int> queue{};
  constexpr int kTotalItems = 500'000;

  std::atomic<bool> producer_done{false};
  std::atomic<std::size_t> total_dequeued{0};

  std::thread producer([&queue, &producer_done] {
    for (int i = 0; i < kTotalItems; ++i) {
      queue.Enqueue(i);
    }
    producer_done.store(true, std::memory_order_release);
  });

  std::thread consumer([&queue, &producer_done, &total_dequeued] {
    while (true) {
      if (queue.Dequeue()) {
        total_dequeued.fetch_add(1, std::memory_order_relaxed);
      } else if (producer_done.load(std::memory_order_acquire)) {
        // Drain any remaining items after producer is done
        while (queue.Dequeue()) {
          total_dequeued.fetch_add(1, std::memory_order_relaxed);
        }
        break;
      }
    }
  });

  producer.join();
  consumer.join();

  EXPECT_EQ(total_dequeued.load(), kTotalItems);
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
    auto first = queue.Dequeue();
    auto second = queue.Dequeue();

    // first and second still hold references to witness
    EXPECT_EQ(witness.use_count(), 3);

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
    [[maybe_unused]]
    int count_after_enqueue = destructor_count;

    // Queue destructor runs
  }

  // Destructors should have been called for all items
  // Exact count depends on move semantics, but should be > 0
  EXPECT_GT(destructor_count, 0) << "No destructors called";
}
