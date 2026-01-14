# An implementation of a lock-free queue in modern C++

![CI](https://github.com/joshuagawley/jglockfree/actions/workflows/ci.yml/badge.svg)

This library contains minimal implementations of 
- the lock-free queue, as described in [Michael and Scott (1996)](https://www.cs.rochester.edu/~scott/papers/1996_PODC_queues.pdf)
- hazard pointers, as described in [Michael (2004)](https://www.cs.otago.ac.nz/cosc440/readings/hazard-pointers.pdf)
- an SPSC lock-free queue using a ring buffer, as described in The Art of Multiprocessor Programming by Herlihy 
and Shavit (2008).

For an explanation of the implementation choices and performance analysis, see [DESIGN.md](DESIGN.md).

## Building, testing, and benchmarking
Building this library requires the following dependencies:
- A C++23 compiler
- CMake

Testing requires GoogleTest and benchmarks require Google Benchmark.

These are provided as submodules, but you can use the system versions if you prefer by setting the 
`USE_SYSTEM_GOOGLETEST` and `USE_SYSTEM_GOOGLEBENCHMARK` CMake options to `ON`.

To clone the repository:
```bash
git clone https://github.com/joshuagawley/jglockfree.git
cd jglockfree
# if you want to use bundled versions of GoogleTest and GoogleBenchmark
git submodule update --init --recursive
```

To build the project in debug mode:
```bash
cmake -B build/Debug
cmake --build build/Debug
```

To test the project:
```bash
ctest --test-dir build/Debug
```

To run the benchmark, you may want to build in release mode:
```bash
cmake -B build/Release -DCMAKE_BUILD_TYPE=Release
cmake --build build/Release
./build/Release/benchmarks/queue_benchmark
```

The `ENABLE_ASAN` and `ENABLE_TSAN` CMake options can be used to enable AddressSanitizer or ThreadSanitizer 
respectively.