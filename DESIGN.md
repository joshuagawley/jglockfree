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
The `Scan()` function uses a stack-allocated array rather than `std::unordered_set` to eliminate heap allocations in the
hot path and because an array has better cache locality than a set.
In addition, we use compile time branching to choose between sorting and then binary search for hazard pointers with a large
number of slots (more than 256), or linear search for fewer slots (256 or less).
For the default 128 slots, this is approximately 25% faster due to eliminated heap allocations and better cache locality,
and the latency growth as we increase the number of threads is similar with 256 slots and 1024 slots:

| Threads | 256 Slots | 1024 Slots |
|---------|-----------|------------|
| 1       | 39.8 ns   | 41.6 ns    |
| 2       | 78.7 ns   | 80.8 ns    |
| 4       | 322 ns    | 276 ns     |
| 8       | 815 ns    | 781 ns     |

We also align each hazard pointer slot to a cache line to avoid false sharing; this improves 
latency by 7%.

## Object pooling
Now we move on to discuss optimizations specific to the M&S queue.
In a naïve implementation, on each enqueue/dequeue operation, we either allocate or delete a node.
Having allocation in the hot path is problematic since the system allocator uses locks when allocating and deallocating
memory, so this would defeat the point of having a lock-free queue.
 
The solution is using a _free list_; each node is allocated from a pool of fixed-size objects.
Each thread has its own free list, as a global free list causes massive contention between threads.
When a node is created, it is allocated from memory already in the pool, and when a node is destroyed, the node's memory
is returned to the pool.


## Limitations
Our implementation has the following limitations:
1. The queue destructor assumes no concurrent operations.
2. With thread local free lists, the producer's thread's free list empties as nodes are allocated
while the consumer's free list keeps growing as nodes are retired. This could result in unbounded memory consumption.


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
| 1       | 13.4 ns           | 9.55 ns       | 13.3 ns         | 9.97 ns     |
| 2       | 80.9 ns           | 34.1 ns       | 25.5 ns         | 30.5 ns     |
| 4       | 283 ns            | 92.9 ns       | 118 ns          | 82.8 ns     |
| 8       | 1593 ns           | 270 ns        | 548 ns          | 250 ns      |

The lock-free queue is generally less performant than the mutex-based queue. This is due to the following reasons:
- The lock-free queue includes hazard pointer overhead: sequentially consistent memory ordering on every protect
operation and periodic scanning of the hazard registry during retirement.
- The mutex queue has a small critical region: just a single `push` or `pop` operation on a `std::queue`, which involves
a handful of pointer manipulations and maybe an amortized allocation. The lock is therefore held for nanoseconds at most,
so threads rarely end up in contention.
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

| Queue                | Throughput       | Time per item |
|----------------------|------------------|---------------|
| M&S lock-free        | 39.0M items/sec  | ~26 ns        |
| SPSC unsignalled     | 37.5M items/sec  | ~27 ns        |
| Mutex                | 34.2M items/sec  | ~29 ns        |
| SPSC with signalling | 22.3M items/sec  | ~45 ns        |


The M&S queue and unsignalled SPSC queue are the best performing implementations, both beating the mutex queue on
throughput.
The SPSC queue's throughput is constrained by cross-core communication; even without signaling, each
operation requires the other core to observe the updated index.
The condition variable signaling overhead reduces throughput by roughly 63%.

### Tail latency
While the lock-free queue has lower throughput than the mutex queue, it provides better tail latency under contention.
At 8 threads (mixed enqueue/dequeue workload):

| Percentile | Lock-Free | Mutex   |
|------------|-----------|---------|
| p50        | 3.7 µs    | 3.7 µs  |
| p99        | 7.5 µs    | 49.9 µs |
| p999       | 111 µs    | 121 µs  |

The lock-free queue's 99th percentile is 6.6 times lower because no thread can block another; contention results in CAS
retries rather than blocking.

For latency-sensitive applications where p99 matters more than throughput, the lock-free
queue is preferable despite its lower average performance.

## Further Reading
- Michael, M. M. and Scott, M. L., "Simple, Fast, and Practical Non-Blocking and Blocking Concurrent Queue Algorithms" (PODC 1996)
- Michael, M. M., "Hazard Pointers: Safe Memory Reclamation for Lock-Free Objects" (IEEE TPDS 2004)
- Thompson, M., Farley, D., Barker, M., Gee, P., and Stewart, A., "Disruptor: High performance alternative to bounded queues for exchanging data between concurrent threads" (LMAX Exchange, 2011)