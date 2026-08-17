# C++ Matching Engine

This project is building a deterministic, single-threaded, in-memory C++ limit
order book using price-time priority.

Milestone 1 Step 4 supports deterministic price-time-priority matching of limit
orders, full and partial fills, synchronous trade generation, efficient
cancellation, and queries for best quotes, active orders, and depth.

The engine is validated with deterministic differential traces against a
separate test-only reference model. AddressSanitizer and UndefinedBehaviorSanitizer
can be enabled for a Debug build with `-DMATCHING_ENGINE_ENABLE_SANITIZERS=ON`.

## Build and test

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

## Benchmarks and profiling

Benchmarks are built by default but are deliberately not registered with CTest.
They use the pinned Google Benchmark 1.8.3 source dependency. Build an
optimized binary with:

```bash
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
./build-release/order_book_benchmark --benchmark_min_time=0.01s --benchmark_repetitions=3 --benchmark_report_aggregates_only=true
```

The suite covers passive insertion, cancellation, a single maker match, same-
and multi-level sweeps, and a deterministic mixed command replay. For a longer
profiling-oriented replay, run:

```bash
./build-release/order_book_profile_workload [replay-count]
perf stat -- ./build-release/order_book_profile_workload
perf record -g -- ./build-release/order_book_profile_workload
```

`perf` is optional and must be installed and permitted by the host. See
[`docs/performance_baseline.md`](docs/performance_baseline.md) for the recorded
baseline and measurement caveats. Disable benchmark targets, for example in a
minimal correctness-only build, with `-DMATCHING_ENGINE_BUILD_BENCHMARKS=OFF`.
