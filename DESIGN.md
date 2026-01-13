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
On x86, acquire/release semantics would have sufficed here, since on x86, stores are not reordered with subsequent 
loads. On ARM, however, such a reordering is permitted. If the reload comes before the store, then a deleter could scan
the hazard registry, see nothing, and delete the node before we reload, causing us to dereference freed memory.

### Implementation notes
The `Scan()` function uses a stack-allocated array with linear search rather than `std::unordered_set`.
For the default 128 slots, this is approximately 25% faster due to eliminated heap allocations and better cache locality.

### Limitations
Our implementation has the following limitations:
1. If a thread exits with nodes in its retired list, those nodes are leaked.
2. The queue destructor assumes no concurrent operations.

## SPSC ring buffer
A ring buffer replaces a linked list with a circular array; this eliminates allocation in the hot path and provides 
better cache locality, at the cost of bounding the size of the queue.
The single-producer-single-consumer (SPSC) variant is the simplest to implement, as single ownership of each index
eliminates the need for compare-and-swap operations.

### Core structure
The buffer uses two atomic indices: `head_` (where the consumer reads) and `tail_` (where the producer writes).
The producer only writes to `tail_` and the consumer only reads from `head_`. This single-writer property means that we
need only use simple loads and stores.

### Full/empty disambiguation
Both "completely full" and "completely empty" could be represented by `head_ == tail_` if we used all slots. However, 
we would then need to use a separate count variable which would introduce contention between producers and consumers on
a shared cache line. To obviate this, we sacrifice one slot to distinguish between the two states; the queue is empty
when `head_ == tail_` and the queue is full when `(tail_ + 1) % size == head_`.

### Memory ordering
The producer loads `head_` with acquire to synchronize with the consumer's release store, ensuring the consumer has
finished reading from a slot before we overwrite it.
The producer stores `tail_` with release to push the data; using release ensures that any consumer that sees the new 
tail is guaranteed to see the data. 
The consumer follows the inverse pattern; loading `tail_` with acquire and storing `head_` with release.
Using relaxed ordering here would allow the CPU to reorder operations such that a reader sees the updated index before
the data is written, and therefore the reader would read uninitialized memory.

### Blocking variants
The blocking `Enqueue()` and `Dequeue()` methods use an adaptive spin-then-block strategy; they spin for 1000 iterations
attempting the lock-free path, then fall back to a mutex-based path if the lock-free path fails.
This optimizes for short waits while remaining efficient for long waits.
The spin loop includes architecture-specific pause hints (`yield` on ARM, `pause` on x86).

### Signaling overhead
To support mixing blocking and non-blocking callers, `TryEnqueue()` and `TryDequeue()` signal condition variables on
success.
This roughly halves throughput compared to the raw lock-free path.
For maximum performance when blocking is not needed, use `TryEnqueueUnsignalled()` and `TryDequeueUnsignalled()`.

## Benchmarks
The following results are wall-clock times per operation, recorded on an Apple M1 processor.

### Mutex-based queue vs lock-free queue
We first compare the performance of the lock-free queue with the mutex-based queue.

| Threads | Lock-Free Enqueue | Mutex Enqueue | Lock-Free Mixed | Mutex Mixed |
|---------|-------------------|---------------|-----------------|-------------|
| 1       | 18.1 ns           | 9.85 ns       | 13.6 ns         | 9.71 ns     |
| 2       | 108 ns            | 35.2 ns       | 35.1 ns         | 32.1 ns     |
| 4       | 241 ns            | 120 ns        | 123 ns          | 78.3 ns     |
| 8       | 1009 ns           | 281 ns        | 682 ns          | 269 ns      |

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

### SPSC vs M&S queue
Now we compare the performance of the two queues above with the SPSC queue.
For the SPSC queue, the only valid configuration is a single producer and single consumer.
We compare sustained throughput (measured in items per second) across all three implementations under this workload:

| Queue                  | Throughput       | Time per item |
|------------------------|------------------|---------------|
| SPSC unsignalled       | 40.2M items/sec  | ~25 ns        |
| Mutex                  | 35.0M items/sec  | ~29 ns        |
| M&S lock-free          | 26.1M items/sec  | ~38 ns        |
| SPSC (with signalling) | 16.3M items/sec  | ~61 ns        |


The raw SPSC ring buffer achieves 40.2M items per second, outperforming both the mutex queue and the M&S
lock-free queue. 
The SPSC queue's throughput is constrained by cross-core communication; even without signaling, each
operation requires the other core to observe the updated index.
The condition variable signaling overhead reduces throughput by roughly 63%.

### Tail latency
While the lock-free queue has lower throughput than the mutex queue, it provides better tail latency under contention.
At 8 threads (mixed enqueue/dequeue workload):

| Percentile | Lock-Free | Mutex   |
|------------|-----------|---------|
| p50        | 3.5 µs    | 4.1 µs  |
| p99        | 10 µs     | 44.5 µs |
| p999       | 173 µs    | 120 µs  |

The lock free queue's 99th percentile is 4.5 times lower because no thread can block another; contention results in CAS
retries rather than blocking.
The higher 999th percentile and maximum latency for the lock-free queue is due to the periodic hazard pointer scanning
per-operation scanning and per-operation memory allocation.

For latency-sensitive applications where p99 matters more than throughput, the lock-free
queue is preferable despite its lower average performance.

## Further Reading
- Michael, M. M. and Scott, M. L., "Simple, Fast, and Practical Non-Blocking and Blocking Concurrent Queue Algorithms" (PODC 1996)
- Michael, M. M., "Hazard Pointers: Safe Memory Reclamation for Lock-Free Objects" (IEEE TPDS 2004)
- Thompson, M., Farley, D., Barker, M., Gee, P., and Stewart, A., "Disruptor: High performance alternative to bounded queues for exchanging data between concurrent threads" (LMAX Exchange, 2011)