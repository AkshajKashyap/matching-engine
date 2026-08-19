# Persistence validation and baseline

## Semantics under test

`DurableEngine` makes a command authoritative by encoding it, appending its
journal frame, and calling `fsync` before applying it to the in-memory
`OrderBook`. A command that the book rejects is still journaled. On recovery,
the engine validates the journal, constructs a fresh book, and replays every
complete record without appending replay frames.

An incomplete final frame is repaired to the last complete frame. Complete
corruption (including malformed headers, frame fields, CRC failures, and
sequence violations) is fatal and is not truncated or skipped. This is tested
across deterministic crash/truncation scenarios; it is not a formal crash
consistency proof.

## Crash and recovery validation

`durable_engine_stress_test.cpp` adds these end-to-end checks:

- Every complete prefix of a 14-command trace recovers to the independent
  reference `OrderBook` state. The test then applies the next command to both
  sides and compares results, including trade IDs.
- Every nonzero proper truncation of a representative 49-byte final submit
  frame is repaired through `DurableEngine::recover()`. Only the two complete
  preceding commands are replayed, and the next append uses sequence 3.
- Payload, checksum, sequence, record-type, and file-header corruption are
  fatal and leave the corrupt file unchanged.
- A 450-block mixed trace executes 3,150 durable operations: passive and
  aggressive orders, partial/full fills, same-price FIFO, cancellations,
  duplicate submissions, invalid prices, and unknown cancellations. It
  performs six destroy/recover checkpoints. At each checkpoint the recovered
  state, full depth, known-order views, invariants, journal count, and next
  sequence are compared with the uninterrupted reference.

The normal suite has 76 tests. The long trace takes roughly nine seconds in the
normal debug-oriented build on the recorded host, so it is intentionally one
deterministic test rather than a randomized soak test.

## Release benchmark method

`durable_engine_benchmark` is deliberately separate from the in-memory
benchmark target. It measures wall-clock time for command execution; journal
creation, fixture setup, and temporary-directory cleanup are outside the timed
region. Durable single-command cases always use the production
append + `fsync` + apply path. Recovery fixtures are canonical encoded journal
bytes so fixture construction does not contaminate scan-and-replay timing.

Recorded command:

```bash
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
./build-release/durable_engine_benchmark \
  --benchmark_min_time=0.01s --benchmark_repetitions=3 \
  --benchmark_report_aggregates_only=true
```

Recorded environment: GCC 13.3, Linux 6.18.33.2-microsoft-standard-WSL2,
`/tmp` on ext4, 12 logical CPUs reported at 2611 MHz, load average 2.55 / 2.13
/ 1.21. Results are host, filesystem, cache, virtualization, and load
dependent; they are not exchange-latency estimates.

### Command-path medians

| Path | In-memory median wall time | Durable median wall time | Durable wall throughput | Durable real-time CV | Approx. slowdown |
| --- | ---: | ---: | ---: | ---: | ---: |
| Passive submit | 793 ns | 2.326 ms | 430 ops/s | 4.0% | 2,930x |
| Aggressive submit | 718 ns | 2.064 ms | 485 ops/s | 2.3% | 2,870x |
| Cancellation | 735 ns | 2.078 ms | 481 ops/s | 3.0% | 2,830x |
| Mixed stream (4 commands) | 738 ns | 6.914 ms | 579 commands/s | 18.4% | 9,370x |

The mixed-stream durable throughput divides the four-command wall time by four.
The `items_per_second` counter emitted by Google Benchmark is CPU-time based;
the table intentionally uses real/wall time because `fsync` latency is the
question being measured.

### Recovery scaling

| Complete records | Median scan + replay wall time | Approx. records/s | Real-time CV |
| ---: | ---: | ---: | ---: |
| 100 | 0.159 ms | 629k | 2.5% |
| 1,000 | 1.501 ms | 666k | 3.2% |
| 10,000 | 19.721 ms | 507k | 24.4% |

The near-linear result is expected from a full journal scan and replay.

### Journal-size scaling

The encoded sizes are fixed: 16-byte file header, 49-byte submit frame, and
32-byte cancel frame. Consequently a journal containing `S` submits and `C`
cancels occupies exactly `16 + 49*S + 32*C` bytes before any incomplete tail.

Examples:

- One benchmark mixed stream (three submits and one cancel): 195 bytes.
- The 3,150-command stress trace (2,250 submits and 900 cancels): 139,066
  bytes.
- Submit-only recovery fixtures: 4,916 bytes at 100 records, 49,016 at 1,000,
  and 490,016 at 10,000.

## Limitations

- `fsync` results vary substantially with the mounted filesystem, WSL2 host
  storage, cache state, virtualization, and contention.
- There is no exactly-once client request protocol; a client timeout after a
  durable commit remains ambiguous.
- There is no group commit, batching, asynchronous journal, snapshot,
  compaction, or multi-process writer protection.

The strict one-`fsync`-per-command policy is intentionally slow here. No
optimization was introduced based on these measurements.
