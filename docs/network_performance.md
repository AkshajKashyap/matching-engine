# Networked durable exchange performance baseline

This is a baseline for the unoptimized, correctness-first exchange service. It
measures the production path with a real loopback TCP client; it is not a
general exchange-latency claim and it does not change durability semantics.

## Method

Build the Release configuration and run:

```bash
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target network_exchange_benchmark
./build-release/network_exchange_benchmark --samples=100 --operations-per-client=32
```

`network_exchange_benchmark` starts one production `ExchangeServer` on
`127.0.0.1` with port `0`, so each invocation receives an ephemeral loopback
port. It uses the real binary codec, nonblocking/poll gateway, bounded queues,
one engine worker, `DurableEngine`, and a new temporary journal for every
workload. One client sends one request and waits for its correlated response.
The timer is `std::chrono::steady_clock` around the client request/response
round trip. The direct cases use an equivalent `DurableEngine` and time only
the relevant `submit` or `cancel` call.

Each case warms ten durable commands before the 100 reported samples. The
mixed trace has four measured commands per block, so it reports 400 samples.
Three independent process invocations were made; the first, preselected
invocation is the detailed table below and the other two are used to describe
variability. Multi-client cases use 32 requests per client, each with its own
TCP connection, released from a barrier together. Their throughput is wall
clock time from release through the final client response; their quantiles are
all client requests flattened together.

Every TCP workload stops the server cleanly, scans the journal for the exact
expected command count (including warmup), and calls `DurableEngine::recover`.
It therefore does not bypass the WAL or silently accept a corrupt benchmark
run. The harness checks the expected accepted/rejected result on each measured
direct operation as well.

## Recorded environment

| Property | Value |
| --- | --- |
| Date | 2026-08-19 |
| Revision | `25e5bc0` before this benchmark-only change |
| Build | CMake Release (`-O3 -DNDEBUG`) |
| Compiler | GCC 13.3.0 (Ubuntu 13.3.0-6ubuntu2~24.04.1) |
| Host | WSL2, Linux 6.18.33.2-microsoft-standard-WSL2 |
| CPU | 13th Gen Intel Core i7-1355U, 6 cores / 12 logical CPUs |
| Journal filesystem | `/tmp` on `/dev/sdd`, ext4 |

WSL2 virtualization, the backing Windows storage stack, filesystem cache
state, CPU frequency policy, scheduling, and host activity all materially
affect `fsync`. GitHub Actions runners are not performance reference machines.
The millisecond results below are measurements of this host and its current
storage behavior, not an intrinsic property of the matching algorithm.

## Workloads

- **Passive submit:** a non-crossing buy at 90.
- **Aggressive submit:** a sell maker at 100 is prepared outside the measured
  interval; the measured buy at 100 fills it.
- **Cancel:** a passive buy is prepared outside the measured interval; the
  measured cancellation removes it.
- **Rejected submit:** a buy with quantity zero. This is intentionally
  journaled and fsynced before its semantic rejection.
- **Mixed:** deterministic repeated blocks: sell 100 × 2, buy 100 × 1,
  passive buy 90 × 1, then cancel that passive buy.

Order IDs and request IDs are deterministic and distinct. Setup commands are
not included in the reported latency but are present in the temporary journal,
as they would be in a real service.

## Direct layers in context

The in-memory order-book baseline remains in
[`performance_baseline.md`](performance_baseline.md): one-maker matching took
about 517--537 ns and cancellation about 539--846 ns across the recorded
sizes. The prior direct durable baseline in
[`persistence_performance.md`](persistence_performance.md) measured 2.064--
2.326 ms for accepted single commands and 6.914 ms for a four-command mixed
stream. That is already roughly three to four orders of magnitude above the
in-memory operation due to per-command append + `fsync`.

The following direct values were remeasured by the new harness in the same
process environment as the TCP comparison. Values are microseconds unless
noted; throughput is based on the sum of timed operation durations.

| Workload | Direct DurableEngine p50 | p95 | p99 | ops/s |
| --- | ---: | ---: | ---: | ---: |
| Passive submit | 2,384.65 | 3,926.23 | 4,399.01 | 389.8 |
| Aggressive submit | 2,211.27 | 4,769.78 | 5,432.89 | 394.6 |
| Cancel | 2,233.00 | 4,265.19 | 6,288.61 | 398.5 |
| Rejected submit | 2,641.54 | 5,745.89 | 7,113.24 | 342.3 |
| Mixed commands (400) | 2,322.17 | 4,257.92 | 5,751.31 | 387.0 |

## Single-client end-to-end latency

The detailed first repetition used 100 samples per single-command workload and
400 samples for mixed. This is the complete request path, from client send to
decoded correlated response.

| Workload | TCP p50 (us) | p95 (us) | p99 (us) | ops/s | TCP p50 / direct p50 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Passive submit | 2,772.96 | 4,386.61 | 6,735.86 | 350.7 | 1.16x |
| Aggressive submit | 2,539.94 | 4,352.68 | 5,725.20 | 368.6 | 1.15x |
| Cancel | 2,407.96 | 3,855.09 | 5,366.52 | 379.1 | 1.08x |
| Rejected submit | 3,414.08 | 5,057.70 | 6,700.18 | 293.9 | 1.29x |
| Mixed commands (400) | 2,625.28 | 4,449.19 | 5,825.67 | 353.1 | 1.13x |

For accepted operations, subtracting equivalent direct p50s gives an
approximate 175--388 us incremental loopback/gateway/queue/response cost
(8--16%). The rejected case was about 773 us (29%) higher in this repetition.
This is deliberately only coarse attribution: `fsync` scheduling and cache
state interact with the worker and network event loop, so the subtraction is
not a precise internal decomposition.

No internal timestamp seam was added. Doing so on the worker hot path would
change the thing being measured. The direct durable measurements are the
service-time proxy; single-client RTT minus that proxy bounds the rest of the
path approximately. Under concurrency, RTT minus direct service time is a
useful coarse indicator of queueing plus gateway scheduling, not a separately
measured queue residence time.

## Saturated multi-client passive-submit throughput

The following first-repetition results use 32 operations per client. Aggregate
throughput does not increase materially after one client, while flattened
client latency grows roughly with concurrency.

| Clients | Aggregate ops/s | p50 (ms) | p95 (ms) | p99 (ms) |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 374.6 | 2.46 | 3.84 | 5.95 |
| 2 | 346.7 | 5.66 | 7.73 | 8.90 |
| 4 | 342.0 | 11.18 | 17.72 | 22.20 |
| 8 | 370.2 | 21.20 | 26.42 | 29.23 |
| 16 | 378.1 | 41.77 | 51.41 | 57.96 |

Across all three repetitions, the passive aggregate-throughput range was
315.7--375.1 ops/s for one client and 338.4--378.1 ops/s for 16 clients.
The 16-client p50 range was 41.77--44.36 ms, while one-client p50 was
2.46--2.94 ms. This is the expected signature of serialized command service:
more clients keep the one worker busy but mostly add waiting, rather than
create command-parallel throughput.

## Variability

The three-repetition range of reported p50s was:

| Workload | Direct p50 range (ms) | TCP p50 range (ms) |
| --- | ---: | ---: |
| Passive submit | 2.281--2.385 | 2.428--3.086 |
| Aggressive submit | 2.211--2.380 | 2.540--2.721 |
| Cancel | 2.233--2.496 | 2.242--2.849 |
| Rejected submit | 2.222--2.642 | 1.984--3.414 |
| Mixed | 2.322--2.576 | 2.220--2.625 |

Tail latency and rejected-command timing are visibly noisier on this WSL2
host. The report therefore emphasizes medians, preserves p95/p99, and does not
select the fastest repetition as the result.

## Durability, recovery, and journal context

The observed plateau near 350--400 commands/s is consistent with the existing
direct measurements: every command, including semantic rejection, performs a
WAL append and synchronous `fsync` before it reaches the in-memory book. The
network layer adds hundreds of microseconds in the single-client case; it does
not explain the millisecond baseline or the multi-client plateau. This is
evidence that synchronous persistence is the dominant cost on this host, not
proof that it is dominant on every filesystem.

Existing recovery measurements remain applicable context: 100, 1,000, and
10,000 records recovered in 0.159 ms, 1.501 ms, and 19.721 ms respectively.
No snapshotting conclusion follows from those small, near-linear numbers.

Journal encoding remains fixed at a 16-byte header, 49 bytes per submit, and
32 bytes per cancel. The representative mixed block is three submits plus one
cancel, or 179 bytes of records (195 bytes including a new journal's header).
There is no compaction or batching in these measurements.

## Conclusion and exactly one next experiment

The end-to-end TCP service adds a modest single-client cost over direct durable
execution, while 2--16 clients demonstrate queueing latency without useful
throughput scaling. The data points to the required synchronous `fsync` as the
first constraint to investigate.

The one justified next engineering experiment is a **correctness-preserving
group-commit prototype**, benchmarked against this baseline with an explicit
durability contract and failure tests. It is not implemented here.
