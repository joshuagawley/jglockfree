// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>
#include <jglockfree/hazard_pointer.h>
#include <jglockfree/queue.h>

#include <atomic>
#include <thread>
#include <vector>

TEST(HazardPointerTest, ProtectReturnsCurrentValue) {
  int node{42};
  std::atomic<int *> source{&node};

  jglockfree::HazardPointer hazard_pointer;
  int *protected_ptr = hazard_pointer.Protect(source);

  EXPECT_EQ(protected_ptr, &node);
  hazard_pointer.Clear();
}

TEST(HazardPointerTest, ProtectDetectsChangeAndRetries) {
  int node_a{1};
  int node_b{2};
  std::atomic<int *> source{&node_a};

  // We can't directly test the retry loop without instrumentation,
  // but we can verify it converges to the final value after a change
  std::thread changer([&] {
    std::this_thread::sleep_for(std::chrono::microseconds(10));
    source.store(&node_b, std::memory_order_release);
  });

  jglockfree::HazardPointer hp;
  const int *result = hp.Protect(source);
  changer.join();

  // Result should be either &node_a (if protect completed before change)
  // or &node_b (if protect saw the change and converged)
  EXPECT_TRUE(result == &node_a || result == &node_b);
  hp.Clear();
}

TEST(HazardPointerTest, IsProtectedReturnsTrueForProtectedPointer) {
  int node = 42;
  std::atomic<int *> source{&node};

  jglockfree::HazardPointer hp;
  (void)hp.Protect(source);

  EXPECT_TRUE(jglockfree::HazardPointer<>::IsProtected(&node));
  hp.Clear();
}

TEST(HazardPointerTest, IsProtectedReturnsFalseAfterClear) {
  int node{42};
  std::atomic<int *> source{&node};

  jglockfree::HazardPointer hp;
  (void)hp.Protect(source);
  hp.Clear();

  EXPECT_FALSE(jglockfree::HazardPointer<>::IsProtected(&node));
}

TEST(HazardPointerTest, IsProtectedReturnsFalseForUnprotectedPointer) {
  int node_a{1};
  int node_b{2};
  std::atomic<int *> source{&node_a};

  jglockfree::HazardPointer hp;
  (void)hp.Protect(source);

  EXPECT_FALSE(jglockfree::HazardPointer<>::IsProtected(&node_b));
  hp.Clear();
}

TEST(HazardPointerTest, ClearIsIdempotent) {
  int node{42};
  std::atomic<int *> source{&node};

  jglockfree::HazardPointer hp;
  (void)hp.Protect(source);
  hp.Clear();
  hp.Clear();  // Should not crash or misbehave

  EXPECT_FALSE(jglockfree::HazardPointer<>::IsProtected(&node));
}

// This test is probabilistic - it may not catch bugs reliably,
// but under ThreadSanitizer it can detect data races
TEST(HazardPointerTest, StressTestProtectClearCycle) {
  constexpr std::size_t kNumIterations{100'000};
  constexpr std::size_t kNumNodes{4};

  std::array<int, kNumNodes> nodes{};
  std::atomic<int *> source{&nodes[0]};
  std::atomic<bool> stop{false};

  // Thread that constantly changes the source
  std::thread mutator([&] {
    std::size_t idx = 0;
    while (not stop.load(std::memory_order_relaxed)) {
      idx = (idx + 1) % kNumNodes;
      source.store(&nodes[idx], std::memory_order_release);
    }
  });

  // Main thread repeatedly protects and clears
  jglockfree::HazardPointer hp;
  for (std::size_t i = 0; i < kNumIterations; ++i) {
    int *ptr = hp.Protect(source);
    // Verify we got a valid pointer from our array
    ASSERT_GE(ptr, &nodes[0]);
    ASSERT_LE(ptr, &nodes[kNumNodes - 1]);
    hp.Clear();
  }

  stop.store(true, std::memory_order_relaxed);
  mutator.join();
}

TEST(HazardPointerTest, StressRetirement) {
  constexpr std::size_t kThreads{8};
  constexpr std::size_t kOpsPerThread{100'000};

  jglockfree::Queue<std::size_t> queue;

  std::vector<std::thread> threads;
  threads.reserve(kThreads);

  for (std::size_t t = 0; t < kThreads; ++t) {
    threads.emplace_back([&queue, t] {
      for (std::size_t i = 0; i < kOpsPerThread; ++i) {
        queue.Enqueue(t * kOpsPerThread + i);
        (void)queue.Dequeue();
      }
    });
  }

  for (auto &t : threads) {
    t.join();
  }
}

TEST(HazardPointerTest, DelayedReader) {
  constexpr std::size_t kIterations{10'000};

  jglockfree::Queue<int> queue;
  std::atomic<std::size_t> iter{0};
  constexpr std::atomic<bool> stop{false};

  auto worker = [&] {
    while (not stop.load(std::memory_order_relaxed)) {
      std::size_t i = iter.fetch_add(1, std::memory_order_relaxed);
      if (i >= kIterations) break;

      queue.Enqueue(1);
      queue.Enqueue(2);
      (void)queue.Dequeue();
      (void)queue.Dequeue();
    }
  };

  std::thread t1(worker);
  std::thread t2(worker);

  t1.join();
  t2.join();
}

TEST(HazardPointerTest, SlotExhaustionThrows) {
  // Use a small slot count to make exhaustion testable
  using SmallHP = jglockfree::HazardPointerN<4>;

  std::vector<std::unique_ptr<SmallHP>> hps;

  // Allocate all 4 slots
  for (std::size_t i = 0; i < 4; ++i) {
    EXPECT_NO_THROW(hps.push_back(std::make_unique<SmallHP>()))
        << "Failed to allocate slot " << i;
  }

  // 5th allocation should throw
  EXPECT_THROW(
      { SmallHP overflow; }, std::runtime_error)
      << "Expected exception when slots exhausted";
}

TEST(HazardPointerTest, SlotReuseAfterDestruction) {
  using SmallHP = jglockfree::HazardPointerN<4>;

  // Allocate all 4 slots
  {
    std::vector<std::unique_ptr<SmallHP>> hps;
    for (std::size_t i = 0; i < 4; ++i) {
      hps.push_back(std::make_unique<SmallHP>());
    }
    // All slots freed when vector goes out of scope
  }

  // Should be able to allocate 4 more
  std::vector<std::unique_ptr<SmallHP>> hps2;
  for (std::size_t i = 0; i < 4; ++i) {
    EXPECT_NO_THROW(hps2.push_back(std::make_unique<SmallHP>()))
        << "Failed to reuse slot " << i;
  }
}

TEST(HazardPointerTest, PartialSlotReuse) {
  using SmallHP = jglockfree::HazardPointerN<4>;

  auto hp1 = std::make_unique<SmallHP>();
  auto hp2 = std::make_unique<SmallHP>();
  auto hp3 = std::make_unique<SmallHP>();
  auto hp4 = std::make_unique<SmallHP>();

  // Destroy hp2, freeing one slot
  hp2.reset();

  // Should be able to allocate one more
  std::unique_ptr<SmallHP> hp5;
  EXPECT_NO_THROW(hp5 = std::make_unique<SmallHP>());

  // But not two more (only one slot was freed)
  EXPECT_THROW({ SmallHP overflow; }, std::runtime_error);
}

TEST(HazardPointerTest, SlotExhaustionMultithreaded) {
  using SmallHP = jglockfree::HazardPointerN<8>;

  std::atomic<std::size_t> successful_allocations{0};
  std::atomic<std::size_t> failed_allocations{0};
  std::atomic<bool> start{false};

  constexpr std::size_t kThreads = 16;  // More threads than slots

  std::vector<std::thread> threads;
  threads.reserve(kThreads);

  for (std::size_t i = 0; i < kThreads; ++i) {
    threads.emplace_back([&] {
      while (not start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }

      try {
        SmallHP hp;
        successful_allocations.fetch_add(1, std::memory_order_relaxed);
        // Hold the slot briefly
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      } catch (const std::runtime_error &) {
        failed_allocations.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  start.store(true, std::memory_order_release);

  for (auto &t : threads) {
    t.join();
  }

  // At most 8 should succeed (the slot count)
  EXPECT_LE(successful_allocations.load(), 8);
  // Total should equal thread count
  EXPECT_EQ(successful_allocations.load() + failed_allocations.load(),
            kThreads);
}
