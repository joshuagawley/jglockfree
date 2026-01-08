# Design
A concurrent queue is a data structure that allows multiple threads to add and remove data in a thread-safe manner.
The use of lock-free data structures is most useful in situations where we want minimal latency (even at the expense of throughput).

## Overview of the M&S algorithm
The basic structure of the M&S concurrent queue is the use of an atomic head and tail pointer, alongside a dummy node.
The dummy node ensures that the head and tail point to different nodes when the queue has items, so enqueuers (working
on the tail) and dequeuers (working on the head) don't compete for the same pointer. When the thread notices that the
queue is in an inconsistent state (e.g. the tail is lagging), it helps fix it before proceeding.

## Memory reclamation
Michael and Scott handwave away memory management ("we assume garbage collection") but since we are using C++, we face
two problems:

**The ABA problem**: A pointer is freed and reallocated to the same address, causing a compare-and-swap operation to 
succeed when it shouldn't. Hazard pointers solve this problem implicitly; a protected node cannot be freed, so its
address cannot be reused while any thread holds a reference to it.

**Use-after-free**: A thread may load a pointer, get preëmpted, and another thread frees that node before the first 
thread dereferences it. Tagged pointers don't help here; the dereference happens _before_ the CAS.

| Thread A (dequeuing)                     | Thread B (dequeuing)           |
|------------------------------------------|--------------------------------|
| Load head → node X                       |                                |
| Get the raw pointer to X                 |                                |
|                                          | Load head -> node X            |
|                                          | Load X.head -> node Y          |
|                                          | CAS succeeds, swings head to Y |
|                                          | X is freed                     |
| Load X.head -> node Y, so use after free |                                |

We solve this using hazard pointers.

## Hazard pointers
Hazard pointers are a mechanism for preventing use-after-free bugs. The concept is that you announce your intention to
read a pointer before you dereference it, and don't free nodes that anyone has announced.

**Reader protocol**:
1. Load the pointer
2. Publish it to a global registry of hazard pointers
3. Re-check that the pointer is still valid (i.e. it hasn't changed at source)
4. Only then dereference it
5. Clear the hazard pointer from the registry when done

**Deleter protocol**:
1. Don't delete the pointer immediately
2. Move retired nodes to a private "to-delete" list
3. Periodically scan all hazard pointers
4. Only free nodes that no thread has announced

This works because if verification succeeds, the node is still reachable. Any deleter that unlinks it _must_ see the 
reader's announcement before freeing.

### Memory ordering
In the `Protect()` function (where we execute the reader protocol), we use sequentially consistent memory ordering when
storing the source pointer in the next available slot and reloading the pointer from source: On x86 processors, we could 
```c++
slot_->store(ptr, std::memory_order_seq_cst);
auto current = source.load(std::memory_order_seq_cst);
```
On x86, acquire/release semantics would have sufficed here, since since on x86, stores are not reordered with subsequent 
loads. On ARM, however, such a reordering is permitted. If the reload comes before the store, then a deleter could scan
the hazard registry, see nothing, and delete the node before we reload, causing us to dereference freed memory.

### Limitations
Our implementation has the following limitations:
1. If a thread exits with nodes in its retired list, those nodes are leaked.
2. The queue destructor assumes no concurrent operations.

## Benchmarks
The following results are wall-clock times per operation, recorded on a Apple M1 processor.

### Benchmark Results

| Threads | Lock-Free Enqueue | Mutex Enqueue | Lock-Free Mixed | Mutex Mixed |
|---------|-------------------|---------------|-----------------|-------------|
| 1       | 14.3 ns           | 9.69 ns       | 13.3 ns         | 10.9 ns     |
| 2       | 105 ns            | 34.0 ns       | 43.1 ns         | 33.4 ns     |
| 4       | 612 ns            | 109 ns        | 188 ns          | 99.8 ns     |
| 8       | 1964 ns           | 318 ns        | 960 ns          | 270 ns      |

The lock-free queue is generally less performant than the mutex-based queue. This is due to the following reasons:
- The lock-free queue includes hazard pointer overhead: sequentially consistent memory ordering on every protect
operation and periodic scanning of the hazard registry during retirement.
- The mutex queue has a small critical region: just a single `push` or `pop` operation on a `std::queue`, which involves
a handful of pointer manipulations and maybe an amortized allocation. The lock is therefore held for nanoseconds at most,
so threads rarely end up in contention.
- The lock-free queue has increased memory allocation overhead: `jglockfree::Queue` allocates a `jglockfree::Node` for 
each item in the queue, whereas `std::queue` amortizes allocation
- On the ARM architecture specifically, compare-and-swap operations are implemented using LL/SC 
(Load-Linked/Store-Conditional), which can spuriously fail under contention, causing more CAS retries.

We mitigate false sharing by using `alignas(std::hardware_destructive_interference_size)` on the head and tail of the
queue to place them on separate cache lines.

Lock-free queues still confer advantages such as non-blocking guarantees, lack of priority inversion, and bounded 
worst-case latency per operation (no thread can block another indefinitely).

## Alternatives
At the cost of bounding the size of the queue, one could use a _ring buffer_ instead of this approach. This uses
a circular array rather than a linked list as the underlying data structure, so we get the obvious cache locality 
benefits and predictable memory usage from the array layout, as well as avoiding allocation in the hot path. 
Ring buffers are common in latency-critical applications such as trading systems.

## Further Reading
- Michael, M. M. and Scott, M. L., "Simple, Fast, and Practical Non-Blocking and Blocking Concurrent Queue Algorithms" (PODC 1996)
- Michael, M. M., "Hazard Pointers: Safe Memory Reclamation for Lock-Free Objects" (IEEE TPDS 2004)