# An implementation of a lock-free queue in modern C++

![CI](https://github.com/joshuagawley/jglockfree/actions/workflows/ci.yml/badge.svg)

This is a minimal implementation of the lock-free queue described in [Michael and Scott (1996)](https://www.cs.rochester.edu/~scott/papers/1996_PODC_queues.pdf).
For an explanation of the algorithm and a performance comparison with a mutex queue, see [DESIGN.md](DESIGN.md).

## Building, testing, and benchmarking
Building this library requires the following dependencies:
- A C++23 compiler
- CMake

Testing requires GoogleTest and benchmarks require Google Benchmark.

The following commands will build the project:
```bash
cmake -B build/Debug
cmake --build build/Debug
```

To test:
```bash
ctest --test-dir build/Debug
```

To benchmark, you may want to build in release mode:
```bash
cmake -B build/Release -DCMAKE_BUILD_TYPE=Release
cmake --build build/Release
./build/Release/benchmarks/queue_benchmark
```