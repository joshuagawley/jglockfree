# Design
A concurrent queue is a data structure that allows multiple threads to add and remove data in a thread-safe manner.
The use of lock-free data structures is most useful in situations where we want minimal latency (even at the expense of throughput).

## Overview of the M&S algorithm
The basic structure of the M&S concurrent queue is the use of an atomic head and tail pointer, alongside a dummy node.
The dummy node ensures that the head and tail point to different nodes when the queue has items, so enqueuers (working
on the tail) and dequeuers (working on the head) don't compete for the same pointer. When the thread notices that the
queue is in an inconsistent state (e.g. the tail is lagging), it helps fix it before proceeding.

The M&S paper handwaves away memory management ("we assume garbage collection") but since we are using C++, we face
tackling the ABA problem. The ABA problem is when a pointer is freed and reallocated to the same address, causing a 
compare-and-swap operation to succeed when it shouldn't. We use tagged pointers to address this issue, although our implementation does potentially
introduce a use-after-free error. A production-quality implementation would use a solution such as a hazard pointer.

## Benchmarks
The following results are wall-clock times per operation, recorded on a Apple M1 processor.

### Benchmark Results

| Threads | Lock-Free Enqueue | Mutex Enqueue | Lock-Free Mixed | Mutex Mixed |
|---------|-------------------|---------------|-----------------|-------------|
| 1       | 13.2 ns           | 9.71 ns       | 12.9 ns         | 10.3 ns     |
| 2       | 77.2 ns           | 33.6 ns       | 57.4 ns         | 31.1 ns     |
| 4       | 255 ns            | 110 ns        | 205 ns          | 142 ns      |
| 8       | 943 ns            | 284 ns        | 991 ns          | 276 ns      |

The lock-free queue is generally less performant than the mutex-based queue. This is due to the following reasons:
- The mutex queue has a small critical region: just a single `push` or `pop` operation on a `std::queue`, which involves
a handful of pointer manipulations and maybe an amortized allocation. The lock is therefore held for nanoseconds at most,
so threads rarely ended up in contention.
- The lock-free queue has increased memory allocation overhead: `jglockfree::Queue` allocates a `jglockfree::Node` for 
each item in the queue, whereas `std::queue` amortizes allocation
- On the ARM architecture specifically, compare-and-swap operations are implemented using LL/SC 
(Load-Linked/Store-Conditional), which can spuriously fail under contention, causing more CAS retries.

Lock-free queues still confer advantages such as non-blocking guarantees, lack of priority inversion, and bounded 
worst-case latency per operation (no thread can block another indefinitely).

## Alternatives
At the cost of bounding the size of the queue, one could use a _ring buffer_ instead of this approach. This uses
a circular array rather than a linked list as the underlying data structure, so we get the obvious cache locality 
benefits and predictable memory usage from the array layout, as well as avoiding allocation in the hot path. 
Ring buffers are common in latency-critical applications such as trading systems.