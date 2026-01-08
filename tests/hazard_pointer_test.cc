// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>
#include <jglockfree/hazard_pointer.h>

#include <jglockfree/queue.h>

#include <atomic>
#include <thread>
#include <vector>

TEST(HazardPointerTest, ProtectReturnsCurrentValue) {
  int node{42};
  std::atomic<int*> source{&node};

  jglockfree::HazardPointer hazard_pointer;
  int *protected_ptr = hazard_pointer.Protect(source);

  EXPECT_EQ(protected_ptr, &node);
  hazard_pointer.Clear();
}

TEST(HazardPointerTest, ProtectDetectsChangeAndRetries) {
  int node_a{1};
  int node_b{2};
  std::atomic<int*> source{&node_a};

  // We can't directly test the retry loop without instrumentation,
  // but we can verify it converges to the final value after a change
  std::thread changer([&] {
    std::this_thread::sleep_for(std::chrono::microseconds(10));
    source.store(&node_b, std::memory_order_release);
  });

  jglockfree::HazardPointer hp;
  const int* result = hp.Protect(source);
  changer.join();

  // Result should be either &node_a (if protect completed before change)
  // or &node_b (if protect saw the change and converged)
  EXPECT_TRUE(result == &node_a || result == &node_b);
  hp.Clear();
}

TEST(HazardPointerTest, IsProtectedReturnsTrueForProtectedPointer) {
  int node = 42;
  std::atomic<int*> source{&node};

  jglockfree::HazardPointer hp;
  hp.Protect(source);

  EXPECT_TRUE(jglockfree::HazardPointer::IsProtected(&node));
  hp.Clear();
}

TEST(HazardPointerTest, IsProtectedReturnsFalseAfterClear) {
  int node{42};
  std::atomic<int*> source{&node};

  jglockfree::HazardPointer hp;
  hp.Protect(source);
  hp.Clear();

  EXPECT_FALSE(jglockfree::HazardPointer::IsProtected(&node));
}

TEST(HazardPointerTest, IsProtectedReturnsFalseForUnprotectedPointer) {
  int node_a{1};
  int node_b{2};
  std::atomic<int*> source{&node_a};

  jglockfree::HazardPointer hp;
  hp.Protect(source);

  EXPECT_FALSE(jglockfree::HazardPointer::IsProtected(&node_b));
  hp.Clear();
}

TEST(HazardPointerTest, ClearIsIdempotent) {
  int node{42};
  std::atomic<int*> source{&node};

  jglockfree::HazardPointer hp;
  hp.Protect(source);
  hp.Clear();
  hp.Clear();  // Should not crash or misbehave

  EXPECT_FALSE(jglockfree::HazardPointer::IsProtected(&node));
}

// This test is probabilistic - it may not catch bugs reliably,
// but under ThreadSanitizer it can detect data races
TEST(HazardPointerTest, StressTestProtectClearCycle) {
  constexpr int kNumIterations{100'000};
  constexpr int kNumNodes{4};

  std::array<int, kNumNodes> nodes{};
  std::atomic<int*> source{&nodes[0]};
  std::atomic<bool> stop{false};

  // Thread that constantly changes the source
  std::thread mutator([&] {
    int idx = 0;
    while (not stop.load(std::memory_order_relaxed)) {
      idx = (idx + 1) % kNumNodes;
      source.store(&nodes[idx], std::memory_order_release);
    }
  });

  // Main thread repeatedly protects and clears
  jglockfree::HazardPointer hp;
  for (auto i : std::views::iota(0, kNumIterations)) {
    int* ptr = hp.Protect(source);
    // Verify we got a valid pointer from our array
    ASSERT_GE(ptr, &nodes[0]);
    ASSERT_LE(ptr, &nodes[kNumNodes - 1]);
    hp.Clear();
  }

  stop.store(true, std::memory_order_relaxed);
  mutator.join();
}

TEST(HazardPointerTest, StressRetirement) {
  constexpr int kThreads{8};
  constexpr int kOpsPerThread{100'000};

  jglockfree::Queue<int> queue;

  std::vector<std::thread> threads;
  threads.reserve(kThreads);

  std::ranges::for_each(std::views::iota(0, kThreads), [&](const auto t) {
    threads.emplace_back([&] {
      std::ranges::for_each(std::views::iota(0, kOpsPerThread), [&](const auto i) {
        queue.Enqueue(t * kOpsPerThread + i);
        queue.Dequeue();
      });
    });
  });
  
  std::ranges::for_each(threads, [](auto& t) { t.join(); });
}

TEST(HazardPointerTest, DelayedReader) {
  constexpr int kIterations{10'000};

  jglockfree::Queue<int> queue;
  std::atomic<int> iter{0};
  constexpr std::atomic<bool> stop{false};

  auto worker = [&] {
    while (not stop.load(std::memory_order_relaxed)) {
      int i = iter.fetch_add(1, std::memory_order_relaxed);
      if (i >= kIterations) break;

      queue.Enqueue(1);
      queue.Enqueue(2);
      queue.Dequeue();
      queue.Dequeue();
    }
  };

  std::thread t1(worker);
  std::thread t2(worker);

  t1.join();
  t2.join();
}

