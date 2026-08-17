# Performance baseline

This is a reproducible baseline for the unoptimized, correctness-first
Milestone 1 order book. It is not a performance claim for other machines.

## Reproduction

```bash
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
./build-release/order_book_benchmark --benchmark_min_time=0.01s --benchmark_repetitions=3 --benchmark_report_aggregates_only=true
./build-release/order_book_profile_workload
```

Each microbenchmark uses three repetitions and at least 10 ms of measured time
per repetition. Creation, initial population, and destruction of the book are
excluded from the targeted insert/cancel/match/sweep timings with Google
Benchmark's pause/resume mechanism. The mixed replay excludes fresh-book setup
but includes every command in the replay. The separate profiling workload
includes fresh-book construction for each replay so that it is useful with
`perf`.

## Recorded environment

| Property | Value |
| --- | --- |
| Date | 2026-08-17 |
| Host | WSL2, Linux 6.18.33.2-microsoft-standard-WSL2 |
| CPU | 13th Gen Intel Core i7-1355U (6 cores / 12 logical CPUs) |
| Compiler | GCC 13.3.0 |
| Configuration | CMake Release: `-O3 -DNDEBUG` |
| Benchmark library | Google Benchmark 1.8.3 (`344117638c8ff7e239044fd0fa7085839fc03021`) |

The benchmark process reported a 0.97 load average. Frequency scaling,
virtualization, allocator state, and host activity can all affect these
nanosecond-level results.

## Representative median CPU-time results

| Workload | 10 | 100 | 1,000 |
| --- | ---: | ---: | ---: |
| Passive insert, distinct levels | 976 ns | 7.42 us | 128.29 us |
| Passive insert, one level | 910 ns | 5.65 us | 87.58 us |
| Cancel, one level | 539 ns | 519 ns | 846 ns |
| Cancel, distinct levels | 679 ns | 615 ns | 981 ns |
| Sweep, one level | 831 ns | 3.14 us | 28.08 us |
| Sweep, distinct levels | 1.06 us | 5.71 us | 74.18 us |

One-maker matching measured 537 ns for a resting ask hit by a buy and 517 ns
for a resting bid hit by a sell. The deterministic mixed replay had these
median CPU throughputs:

| Blocks | Operations/replay | Operations/s | Trades/s |
| --- | ---: | ---: | ---: |
| 32 | 128 | 15.77 M | 5.67 M |
| 256 | 1,024 | 12.23 M | 4.41 M |
| 1,024 | 4,096 | 13.21 M | 4.74 M |

The profile workload (4,000 replays of the 256-block trace) completed
4,096,000 operations and 1,476,000 trades in 0.37 elapsed seconds, with a
reported maximum resident set of 3,748 KiB. Its deterministic checksum was
zero.

## Interpretation and next measurements

The expected scale difference is visible: crossing 1,000 distinct levels takes
about 2.6 times the median CPU time of crossing 1,000 makers at one level
(74.18 us versus 28.08 us). Distinct-level passive insertion is likewise more
expensive at 1,000 orders (128.29 us versus 87.58 us). Cancellation remains
sub-microsecond at the measured sizes, consistent with locator-based removal,
although the cleanup of an emptied level still adds map work.

`perf` was not installed on the recorded host, so no hardware-counter or
call-graph attribution is claimed. On a host where it is available, start with:

```bash
perf stat -- ./build-release/order_book_profile_workload
perf record -g -- ./build-release/order_book_profile_workload
perf report
```

Optimization hypotheses to validate with those profiles are:

1. Red-black-tree level lookup and level removal should dominate the gap between
   one-level and many-level workloads.
2. `std::list` node traversal/erasure and active-locator hash-map erasure may
   dominate large sweeps.
3. The matching implementation's preflight traversal for trade-ID capacity may
   duplicate work in a large sweep.

The first optimization experiment I would run, without committing to it yet, is
to measure a safe fast path that avoids the full preflight traversal when the
trade-ID counter has ample headroom. It must preserve the existing overflow
behavior and be justified by a profile; no such change is part of this
baseline.

## Correctness guardrail

Benchmark targets are separate from CTest. Before treating a performance change
as valid, run the normal test suite and the existing ASan/UBSan configuration;
the latter disables leak detection only because this WSL test environment runs
under ptrace, which LeakSanitizer does not support.

## Function-level investigation (Step 7)

### Workload coverage and decomposition

The profiling workload now validates its own command stream before entering the
hot loop. It aborts if the trace does not contain passive submissions,
cancellations, same-level matches, multi-level matches, full fills, and partial
fills. The optional `replay-count` argument extends the trace without changing
its deterministic commands or emitting output in the hot loop.

The benchmark suite gained these isolated Release measurements:

- `BM_PassiveInsertExistingLevel`: inserts one passive order into an already
  populated FIFO level.
- `BM_PassiveInsertNewLevel`: inserts one passive order that creates a level in
  an already populated price tree.
- `BM_CancelKeepLevel`: removes an order from a FIFO level that remains.
- `BM_CancelRemoveLevel`: removes the only order at a level, causing level
  removal from the price tree.

All setup remains paused, as in the original microbenchmarks. The following
three-repetition, 5 ms-minimum run had a load average of approximately 2.1, so
it is useful for directional comparison rather than replacing the lower-load
Step 6 baseline:

| Isolated path, median CPU time | 10 existing orders/levels | 100 | 1,000 |
| --- | ---: | ---: | ---: |
| Insert into existing level | 880 ns | 939 ns | 1.30 us |
| Insert new level | 1.08 us | 1.08 us | 2.94 us |
| Cancel and keep level | 756 ns | 797 ns | 1.40 us |
| Cancel and remove level | 1.17 us | 1.37 us | 1.67 us |

### Tool availability and method

On this WSL host, `perf --version` and `valgrind --version` both failed with
`command not found`; therefore no `perf stat`, `perf record`, or Callgrind
result exists. GNU `gprof` 2.42 was available and used as an additional,
best-effort fallback.

The first `gprof` build used `-O2 -g -DNDEBUG -pg` and ran 40,000 deterministic
mixed replays (40,960,000 operations; 7.50 wall-clock seconds). It collected
2.19 seconds of sampling time. A second diagnostic build added `-fno-inline` so
that `preview_match()` and `match()` had distinct symbols; it ran 2,000 replays
in 18.08 wall-clock seconds and collected only 1.23 seconds of samples.

`gprof`'s sampling clock under WSL did not track elapsed time reliably, and
`-fno-inline` substantially changes code generation. Its percentages are
relative attribution only: they are neither Release timings nor hardware
counter results. Reproduce the diagnostic build on a native Linux host with:

```bash
cmake -S . -B build-gprof -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS_RELWITHDEBINFO='-O2 -g -DNDEBUG -pg -fno-inline' \
  -DCMAKE_EXE_LINKER_FLAGS=-pg
cmake --build build-gprof
mkdir -p /tmp/matching-engine-gprof
cd /tmp/matching-engine-gprof
/path/to/build-gprof/order_book_profile_workload 2000
gprof /path/to/build-gprof/order_book_profile_workload gmon.out
```

### Observed measurements

- Creating a new level cost about 2.3 times an existing-level insertion at
  1,000 existing entries (2.94 us versus 1.30 us), although the existing-level
  result had a 35% coefficient of variation on this loaded host.
- Removing a level was slower than retaining one at every size in that run:
  1.17 versus 756 ns at 10 and 1.67 versus 1.40 us at 1,000.
- The Step 6 1,000-maker sweep remained 28.08 us at one level and 74.18 us
  across 1,000 levels on its lower-load run; the latter is 2.6 times slower.
- In the optimized `gprof` trace, `seen_order_ids` insertion and its
  `_M_rehash` implementation accounted for 13.70% and 14.61% of sampled time,
  respectively. `OrderBook::submit` and `OrderBook::cancel` were 23.29% and
  20.55% self time, but are high-level wrappers, not root causes.

### Hotspot evidence

- Hash growth is the clearest measured library-level cost: the optimized trace
  observed 280,007 `seen_order_ids` rehash calls across 40,000 fresh-book
  replays. This is amplified by the profiling workload's intentional
  reconstruction of a book for each replay.
- The no-inline diagnostic exposed the ask-side `preview_match()` at 3.25%
  self time (four 10 ms samples) and the ask-side `match()` at 1.63% (two
  samples); the bid-side instances had zero and one samples. This confirms both
  paths execute, but the sample count is too low and the build too distorted to
  establish `preview_match()` as a substantial whole-workload bottleneck.
- The diagnostic report included `std::map::erase` (1.63%), `try_emplace`
  (0.81%), `std::list::_M_insert` (2.44%), and active-order index erase
  (1.63%). These support the targeted measurements that level creation/removal
  has a real cost, but do not prove that any one container dominates overall.

### Hypotheses, not conclusions

- Price-tree node creation/removal and traversal likely explain part of the
  new-level and multi-level gap.
- List-node and hash-index allocations are present during order insertion and
  full fills, but this evidence does not show that list traversal is the chief
  same-level sweep cost.
- `preview_match()` necessarily makes a read-only maker traversal before the
  mutating traversal. It is a plausible sweep optimization target, but current
  WSL `gprof` evidence does not justify calling it the primary bottleneck.

### Exactly one recommended first experiment

Measure an internal initial `seen_order_ids.reserve(1024)` capacity hint in the
`OrderBook::Impl` constructor. This is a small, reversible, semantics-neutral
experiment directly supported by the measured rehash hotspot; it does not
replace a container or alter matching/cancellation logic.

It should improve `BM_PassiveInsertUniqueLevels`,
`BM_PassiveInsertSameLevel`, and `BM_MixedReplay`, while same- and multi-level
sweep *matching* timings should be effectively unchanged because their maker
setup is paused. Success means a material reduction in mixed-replay and
passive-insert CPU time with no regression in sweep/match timings and all 40
correctness tests still passing. It may trade a small fixed amount of memory
and construction work for fewer early rehashes, so it must be measured before
adoption. It is not implemented here.

## Controlled reserve experiment (Step 8)

### Hypothesis and tested change

The experiment added this private constructor-time hint, and no other
production change:

```cpp
static constexpr std::size_t initial_seen_order_id_capacity = 1'024;
seen_order_ids.reserve(initial_seen_order_id_capacity);
```

The intended mechanism was to avoid early bucket reallocations while accepted
lifetime order IDs accumulate. The expected beneficiaries were bulk passive
insertion and mixed replay; isolated sweeps and matching were expected to be
largely unaffected because setup is paused.

All measurements below used the same GCC 13.3 Release build and Google
Benchmark configuration: five repetitions, 5 ms minimum measured time, and
median CPU time. Positive percentages use
`(baseline - reserve) / baseline * 100`; negative values are regressions.

### First A/B run

The raw first before/after comparison was:

| Workload | Baseline median | Reserve median | Change | Baseline / reserve CPU CV |
| --- | ---: | ---: | ---: | ---: |
| Passive unique levels, 10 | 2.21 us | 1.10 us | +50.2% | 11.4% / 5.4% |
| Passive unique levels, 100 | 16.24 us | 8.43 us | +48.1% | 15.2% / 1.5% |
| Passive unique levels, 1,000 | 279.51 us | 95.29 us | +65.9% | 2.2% / 1.3% |
| Passive one level, 10 | 2.07 us | 892 ns | +56.9% | 19.7% / 3.0% |
| Passive one level, 100 | 11.46 us | 6.39 us | +44.3% | 4.5% / 9.0% |
| Passive one level, 1,000 | 136.08 us | 53.31 us | +60.8% | 4.0% / 5.8% |

The newly isolated submission paths did not show the predicted benefit:

| Workload | Baseline median | Reserve median | Change | Baseline / reserve CPU CV |
| --- | ---: | ---: | ---: | ---: |
| Insert existing level, 10 | 778 ns | 1.38 us | -77.0% | 10.5% / 12.3% |
| Insert existing level, 100 | 637 ns | 1.31 us | -104.9% | 6.9% / 13.0% |
| Insert existing level, 1,000 | 791 ns | 1.65 us | -108.1% | 6.6% / 18.9% |
| Insert new level, 10 | 648 ns | 924 ns | -42.6% | 5.6% / 12.2% |
| Insert new level, 100 | 705 ns | 1.05 us | -49.2% | 5.5% / 11.1% |
| Insert new level, 1,000 | 1.07 us | 2.09 us | -95.9% | 10.5% / 23.9% |

The complete workload and the supposedly unaffected matching cases also moved
in the wrong direction in that first pair:

| Workload | Baseline median | Reserve median | Change | Baseline / reserve CPU CV |
| --- | ---: | ---: | ---: | ---: |
| Mixed replay, 32 blocks (96 accepted IDs) | 9.15 us | 16.44 us | -79.7% | 11.2% / 13.1% |
| Mixed replay, 256 blocks (768 IDs) | 90.57 us | 141.41 us | -56.1% | 10.6% / 14.8% |
| Mixed replay, 1,024 blocks (3,072 IDs) | 384.86 us | 617.10 us | -60.3% | 6.5% / 25.2% |
| Same-level sweep, 1,000 makers | 27.68 us | 35.86 us | -29.5% | 9.7% / 6.1% |
| Multi-level sweep, 1,000 makers | 73.86 us | 90.21 us | -22.1% | 6.0% / 13.7% |
| One maker, incoming buy | 406 ns | 799 ns | -96.8% | 4.1% / 5.0% |
| One maker, incoming sell | 410 ns | 801 ns | -95.4% | 4.5% / 9.7% |

The mixed trace supplies the requested scale check: 96 and 768 accepted IDs
are below the 1,024 hint, while 3,072 is above it. It did not show a stable
benefit at any of those lengths. The original passive workloads do create many
accepted IDs and did show an apparent improvement, but that result did not
survive the repeatability control below.

### Repeatability control and decision

After reverting the hint, a one-maker buy measured 794 ns, essentially the
same as the reserve build's 799 ns rather than its original 406 ns baseline.
The reverted 256-block mixed replay measured 125.23 us versus 141.41 us with
the hint; their 13–15% coefficients of variation make that difference
inconclusive. A second reserve build produced 282.89 us for 1,000 unique-level
passive inserts and 203.84 us for 1,000 same-level inserts, instead of the
first reserve run's 95.29 and 53.31 us. This is roughly a threefold shift for
the same binary configuration with similarly low reported load.

**Decision: INCONCLUSIVE; the reserve was reverted.** WSL frequency scheduling
and host variability prevented a repeatable attribution of the apparent bulk
insertion win, while the relevant mixed workload did not show a reliable gain.
Keeping an unconditional memory policy on that evidence would be speculative.

The fixed hint also allocates the bucket array when every `OrderBook` is
created, even when it remains empty or receives only a few IDs. With the
default load factor, libstdc++ typically chooses a bucket count near 1,024,
which is roughly 8 KiB of bucket pointers on a 64-bit process plus allocator
metadata; the exact amount is implementation-dependent. The constant is an
arbitrary workload assumption, and books that outgrow it still rehash.

The experiment did not change matching semantics or public APIs. After the
final revert, the normal default test configuration and the Debug ASan/UBSan
configuration both passed all 40 tests. Any future retry should use
a quieter native host, fixed CPU affinity/frequency where available, longer
runs, and interleaved A/B builds before reconsidering this capacity policy.
