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

## Opportunistic group-commit experiment (Step 2)

The worker now blocks for one admitted request and immediately drains only
already-queued FIFO requests up to `max_durable_batch_size` (default `1`). It
does not wait, sleep, or use a batching timer. `JournalWriter` pre-encodes each
record, writes frames individually in queue order, performs one `fsync`, and
only then `DurableEngine` applies commands and returns ordered results. The WAL
format is unchanged: a failed/unacknowledged batch is not atomic, and recovery
continues to treat whatever complete valid prefix it observes as authoritative.

Consequently, a successful response implies its command passed the batch's
successful durability barrier. Conversely, no response or EngineUnavailable
does not prove that the command cannot appear during later recovery after an
ambiguous write or sync failure. A write/sync failure fail-stops the writer,
engine, and gateway without applying the live batch. An unexpected post-sync
application exception also fail-stops and withholds every batch response;
recovery reconstructs the authoritative state.

### Initial Release control matrix

This first matrix is a short one-repetition smoke measurement (16 operations
per client, not a replacement for the three-repetition 100-sample baseline).
It is included to validate the experiment and guide the longer follow-up run.
Cells are aggregate operations/s with flattened p50 latency in milliseconds.

| Batch cap | 1 client | 2 clients | 4 clients | 8 clients | 16 clients |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 269 / 3.79 | 282 / 6.81 | 293 / 13.07 | 289 / 26.53 | 325 / 45.02 |
| 2 | 286 / 3.57 | 362 / 5.37 | 684 / 5.87 | 687 / 10.98 | 710 / 22.14 |
| 4 | 298 / 3.33 | 381 / 5.29 | 645 / 6.24 | 1,229 / 6.42 | 1,351 / 11.77 |
| 8 | 326 / 3.16 | 364 / 5.60 | 737 / 5.45 | 1,381 / 5.63 | 2,676 / 5.89 |
| 16 | 319 / 3.00 | 350 / 5.46 | 730 / 5.56 | 1,352 / 5.82 | 2,846 / 5.19 |
| 32 | 321 / 2.86 | 403 / 4.75 | 739 / 5.11 | 1,602 / 4.85 | 3,017 / 5.21 |

Worker-observed mean occupancy at 16 clients was 1.0, 2.0, 4.0, 7.76, 8.0,
and 8.0 for caps 1, 2, 4, 8, 16, and 32 respectively. The cap-32 histogram
was sixteen batches of four and sixteen batches of twelve; the system's
admission/scheduling pattern, not the configured cap, limited occupancy to
eight on average. At one client all caps observed occupancy one.

The cap-1 control reproduces the earlier ~340--380 ops/s plateau within normal
WSL/filesystem variability. Under 16 concurrent clients, caps 8--32 improved
the short-run throughput by roughly 8--9x while keeping p50 near 5--6 ms rather
than the prior ~42 ms queueing regime. This supports the fsync-amortization
hypothesis, but the short matrix is noisy and does not yet justify changing the
default. The default remains batch cap 1 pending three-repetition long runs,
including p95/p99 and selected mixed workloads.

## Final controlled group-commit evaluation (Step 2)

This section is the final controlled evaluation of the already-implemented
opportunistic group-commit path. It supersedes neither the historical baseline
above nor the short smoke matrix: the historical numbers remain historical, and
the smoke data remains useful only as an early correctness/performance check.

### Hypothesis, implementation, and durability contract

The hypothesis was that concurrent admitted commands would already be queued,
allowing one successful `fsync` to cover several FIFO WAL records and therefore
amortize the dominant persistence cost. The control is the same new
implementation at batch cap `1`, not the older pre-batching binary.

The worker blocks for the first request, immediately drains only already queued
requests up to the configured cap, writes each WAL frame in FIFO order, calls
exactly one `fsync`, and only then applies commands and sends ordered results.
There is no batching timer, no extra worker, and journal v1 is unchanged.
Every success response still follows the successful `fsync` covering its
command. A cap above one changes grouping, not the response durability
contract; a failed/unacknowledged group is not claimed to be atomic.

### Environment and method

| Property | Value |
| --- | --- |
| Date | 2026-08-20 |
| Build | Fresh CMake `Release` build (`-O3 -DNDEBUG`) |
| Compiler | GCC 13.3.0 (Ubuntu 13.3.0-6ubuntu2~24.04.1) |
| Host | WSL2, Linux 6.18.33.2-microsoft-standard-WSL2 |
| CPU | 13th Gen Intel Core i7-1355U, 6 cores / 12 logical CPUs |
| Journal path/filesystem | Per-run `mkdtemp` journal under `/tmp`; ext4 on `/dev/sdd` |
| Reported samples | 128 requests per client per primary cell; 128 mixed commands per client (32 four-command blocks) |
| Repetitions | Three serial independent process invocations per cap/workload |

Single-client direct/TCP cases retain the harness's ten-command warmup. The
concurrent matrices intentionally do not add a warmup phase: clients connect,
wait at a barrier, and then begin the saturated measured stream together. Each
matrix invocation cleanly stops the exchange, validates the exact journal record
count, and calls recovery. Latency quantiles are flattened per-request RTTs;
throughput is aggregate wall-clock completion rate.

### Primary: saturated passive submits

Each table cell below is the median of three repetitions; `ops/s range` is the
minimum--maximum repetition result rather than an average. `occ` is median
worker-observed commands per batch. The raw batch histograms corroborated the
occupancies: cap 1 was always `1:N`; saturated cap 2 was almost entirely `2:N`;
at 8 and 16 clients, cap 4 reached four and caps 8--32 reached eight (with only
small boundary batches). Thus a configured cap is not assumed to be achieved.

| Cap | Clients | Median ops/s (range) | p50 ms | p95 ms | p99 ms | occ |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 1 | 234.7 (199.1--375.6) | 3.95 | 6.43 | 7.92 | 1.00 |
| 1 | 2 | 217.3 (203.9--406.1) | 8.45 | 13.11 | 15.54 | 1.00 |
| 1 | 4 | 214.5 (204.6--349.9) | 18.14 | 24.06 | 26.15 | 1.00 |
| 1 | 8 | 244.3 (176.0--335.0) | 32.65 | 39.59 | 46.89 | 1.00 |
| 1 | 16 | 249.8 (232.7--396.7) | 63.03 | 81.74 | 100.94 | 1.00 |
| 2 | 1 | 243.1 (226.8--248.3) | 3.92 | 5.96 | 7.54 | 1.00 |
| 2 | 2 | 221.8 (219.2--247.0) | 8.58 | 13.12 | 21.63 | 1.00 |
| 2 | 4 | 467.8 (461.5--472.9) | 8.51 | 10.86 | 13.83 | 2.00 |
| 2 | 8 | 465.3 (434.6--477.1) | 16.80 | 23.26 | 27.08 | 2.00 |
| 2 | 16 | 479.0 (463.5--479.3) | 33.41 | 43.89 | 50.91 | 2.00 |
| 4 | 1 | 254.0 (194.5--275.2) | 3.71 | 5.79 | 7.08 | 1.00 |
| 4 | 2 | 254.4 (180.7--284.6) | 7.68 | 11.22 | 14.18 | 1.02 |
| 4 | 4 | 379.8 (372.2--593.0) | 10.12 | 14.23 | 15.64 | 2.01 |
| 4 | 8 | 783.1 (729.2--1,016.1) | 10.32 | 13.46 | 14.88 | 4.00 |
| 4 | 16 | 782.3 (646.3--955.0) | 20.31 | 24.86 | 27.45 | 3.99 |
| 8 | 1 | 404.5 (384.1--410.4) | 2.15 | 4.19 | 5.79 | 1.00 |
| 8 | 2 | 461.5 (376.2--480.2) | 4.14 | 5.85 | 6.40 | 1.00 |
| 8 | 4 | 842.3 (724.8--917.7) | 4.62 | 6.49 | 7.95 | 2.01 |
| 8 | 8 | 1,686.3 (1,433.7--1,708.1) | 4.53 | 7.39 | 8.52 | 4.00 |
| 8 | 16 | 2,941.0 (2,782.5--3,814.3) | 4.92 | 8.24 | 10.90 | 7.97 |
| 16 | 1 | 429.5 (350.2--432.5) | 2.11 | 3.93 | 4.78 | 1.00 |
| 16 | 2 | 476.9 (379.5--492.3) | 3.90 | 6.41 | 8.18 | 1.00 |
| 16 | 4 | 896.8 (757.1--985.6) | 3.97 | 7.12 | 8.18 | 2.01 |
| 16 | 8 | 1,876.2 (1,481.5--1,882.5) | 3.85 | 6.28 | 7.57 | 4.00 |
| 16 | 16 | 3,498.3 (2,654.6--3,529.7) | 4.36 | 6.65 | 8.72 | 8.00 |
| 32 | 1 | 384.4 (354.5--460.2) | 2.33 | 4.43 | 5.31 | 1.00 |
| 32 | 2 | 430.2 (426.6--443.7) | 4.45 | 6.65 | 8.17 | 1.00 |
| 32 | 4 | 873.0 (871.7--931.1) | 4.39 | 6.51 | 8.05 | 2.01 |
| 32 | 8 | 1,723.5 (1,653.6--1,771.3) | 4.45 | 6.37 | 8.40 | 4.00 |
| 32 | 16 | 3,261.7 (2,911.4--3,322.5) | 4.70 | 6.78 | 8.79 | 8.00 |

The 16-client control is 249.8 ops/s median (232.7--396.7), lower and noisier
than the historical pre-group-commit 340--380 ops/s plateau but overlapping it
in the fastest repetition. That historical result is not a same-run control;
the new cap-1 measurement above is the primary comparator.

### Secondary: saturated mixed stream

The mixed stream is the existing deterministic four-command block: sell 100 ×
2, crossing buy × 1, passive buy 90 × 1, and cancellation. There are 128
commands/client (32 blocks), three repetitions, and the same response/journal
validation as the passive matrix.

| Cap | Clients | Median ops/s (range) | p50 ms | p95 ms | p99 ms | occ |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 4 | 453.5 (438.2--470.2) | 8.54 | 12.68 | 14.73 | 1.00 |
| 1 | 16 | 424.7 (398.6--466.1) | 37.06 | 47.09 | 54.46 | 1.00 |
| 8 | 4 | 914.2 (888.9--1,039.0) | 4.12 | 6.11 | 7.13 | 2.02 |
| 8 | 16 | 3,038.3 (3,023.0--3,593.0) | 4.82 | 7.97 | 9.88 | 7.97 |
| 32 | 4 | 849.2 (836.2--853.6) | 4.51 | 6.71 | 9.20 | 2.00 |
| 32 | 16 | 3,212.5 (3,202.4--3,500.1) | 4.69 | 7.63 | 9.55 | 8.00 |

### Interpretation and recommendation

The hypothesis is supported. A single client has observed occupancy one for
every cap, so there is no intentional waiting to fill a batch and little
evidence of a single-client grouping benefit. As client count rises, commands
are already queued: occupancy moves from roughly two at four clients to four
at eight and eight at sixteen. The matching throughput increases correspondingly.

At 16 passive clients, cap 8 is an 11.8× median improvement over the same-run
cap-1 control (2,941 vs 250 ops/s); cap 16 reaches 3,498 ops/s, or 14.0×. The
cap-8 and cap-16 throughput ranges overlap, and both achieve only eight actual
commands per batch. Cap 32 also remains at occupancy eight and has no repeatable
throughput advantage over cap 8 or 16. The largest repeatable gain is therefore
about 14× at cap 16, while the practical diminishing-return point is an
achieved occupancy of eight rather than a configured cap above eight.

Tail behavior improves materially under saturation because queueing behind one
fsync per command disappears: passive cap 1 at 16 clients has median p99
100.94 ms, versus 10.90 ms at cap 8, 8.72 ms at cap 16, and 8.79 ms at cap 32.
The cap-8 first repetition had a 21.56 ms p99 outlier, so small cap-8/cap-16
tail differences should not be over-read. Mixed workloads show the same
direction: 16-client p50 falls from 37.06 ms at cap 1 to about 4.7--4.8 ms at
caps 8 and 32.

**Recommendation: change the default `max_durable_batch_size` to 8.** The gain
over cap 1 is large and repeatable, observed occupancy explains it, the durable
success contract is unchanged, and no timer is introduced. Eight captures the
observed useful concurrency while avoiding a larger configured bound that this
workload does not fill. This report makes the recommendation only; it does not
change the production default.

Remaining limitations: this is one WSL2 host with highly variable synchronous
filesystem timing; no CPU pinning, cache dropping, or background-load control
was attempted. The benchmark is closed-loop request/response traffic and does
not represent a production client distribution or crash-injected storage run.
More bare-metal/filesystem measurements are needed before treating cap 8 as a
universal deployment default.
