# toyredis

A Redis-style in-memory key/value server: a single-threaded, non-blocking
event loop (`poll()`-based) in front of a custom hashtable, an AVL-tree-backed
sorted set, a min-heap for key expiry, and a worker thread pool for offloading
expensive deletes. A benchmark harness and memory sampler measure throughput,
latency percentiles, and resident set size under load.


## Layout

```
toyredis/
├── Makefile               # builds server, client, benchmark
├── final/
│   ├── include/           # avl.h, hashtable.h, heap.h, list.h, thread_pool.h, zset.h, common.h
│   ├── src/core/          # avl.cpp, hashtable.cpp, heap.cpp, thread_pool.cpp, zset.cpp
│   ├── src/server.cpp     # the server (event loop + command dispatch)
│   └── client/
│       ├── client.cpp     # interactive single-shot client
│       └── benchmark.cpp  # concurrent pipelined benchmark
├── scripts/
│   ├── memtrack.sh        # samples server RSS/VSZ from /proc into a CSV
│   └── run_bench.sh       # starts server, runs benchmark, samples memory
└── tests/
    └── test_cmds.py       # functional smoke test (get/set/del/keys/pexpire/pttl/zset)
```


## Build

Requires `g++` (or any GCC-compatible compiler) and `make`. The `container_of`
macro uses GCC's `typeof` and statement-expression extensions, so the Makefile
compiles with `-std=gnu++17`.

```
make            # builds server, client, benchmark into ./bin/
make clean
```

## Design

- **Event loop.** A single thread runs `poll()` over all connected sockets;
  no per-connection thread or blocking read/write.
- **Hashtable.** Open addressing with progressive (incremental) rehashing, so
  a resize never causes a single big latency spike.
- **Sorted set.** An AVL tree keyed on `(score, name)` for ordered range
  queries, plus a hashtable keyed on `name` for O(1) `ZSCORE` lookups.
- **TTLs.** A min-heap of expiry deadlines drives `PEXPIRE` / `PTTL` and lazy
  eviction from the event loop's timeout.
- **Idle-connection reaping.** An intrusive doubly-linked list orders
  connections by last-active time so idle sockets can be closed in O(1) off
  the head of the list.
- **Thread pool.** Deleting a very large sorted set is O(n); that work is
  handed off to a worker pool so it doesn't stall the single-threaded event
  loop for other clients.

### Wire protocol

**Request**

```
[u32 total_payload_len]
[u32 num_strings]
  [u32 string_len][bytes]   -- repeated num_strings times
```

**Response** — a single typed value, prefixed by total length.

| Tag | Name | Body                                          |
|-----|------|-----------------------------------------------|
| 0   | NIL  | (empty)                                       |
| 1   | ERR  | `[i32 code][u32 msglen][msg bytes]`           |
| 2   | STR  | `[u32 len][bytes]`                            |
| 3   | INT  | `[i64]`                                       |
| 4   | DBL  | `[f64]`                                       |
| 5   | ARR  | `[u32 nelem]` then `nelem` typed values       |

### Commands the final server speaks

```
get <key>
set <key> <value>
del <key>
keys
pexpire <key> <ms>
pttl <key>
zadd <key> <score> <member>
zrem <key> <member>
zscore <key> <member>
zquery <key> <min_score> <name_lo> <offset> <limit>
```


## Running the server

In one terminal:

```
./bin/server
```

In another:

```
./bin/client set hello world
./bin/client get hello
./bin/client zadd zs 1.5 alice
./bin/client zadd zs 2.5 bob
./bin/client zquery zs 0 "" 0 10
./bin/client pexpire hello 60000
./bin/client pttl hello
```

Functional smoke test (server must be running on `:1234`):

```
make test
```


## Quantifying the metric

The benchmark harness measures two things:

1. **Performance** — throughput (ops/sec) and latency distribution (p50/p90/p99/max),
   per command type.
2. **Memory** — server resident set size (`VmRSS` from `/proc/<pid>/status`),
   sampled every 100 ms while the workload runs.

### One-shot run

```
make bench
# or with custom load:
./scripts/run_bench.sh -c 100 -n 200000 -P 32 -d 64
```

That script:

1. Starts `./bin/server` in the background and captures its PID.
2. Launches `./scripts/memtrack.sh` against that PID, writing
   `bench_out/memtrack.csv`.
3. Runs `./bin/benchmark` with whatever flags you passed; CSV output goes
   to `bench_out/bench.csv`.
4. Stops the sampler, kills the server, and writes `bench_out/summary.txt`.

### Benchmark methodology

The benchmark client opens **C** concurrent TCP connections, each driven by its
own thread. Each connection pipelines up to **P** requests at a time (sends a
batch, then drains the responses). The total **N** ops are partitioned across
threads so every thread owns a disjoint range of keys — meaning the SET, GET,
and DEL phases are deterministic and never contend with each other.

It runs three phases in order:

1. **SET** phase — populates `N` keys, each with a value of size `d` bytes.
2. **GET** phase — reads each of those `N` keys once.
3. **DEL** phase — deletes them.

For each phase it reports:

- Total ops, wall-clock seconds, and **ops/sec** (throughput).
- Per-op latency percentiles in microseconds. Latency is sampled per
  fully-drained pipeline window: a window of P pipelined ops takes `t` ns, so
  the per-op latency in that window is `t / P`. This avoids the overhead of
  timestamping every individual request, which would distort the hot path.

Default knobs:

| Flag | Meaning                                  | Default      |
|------|------------------------------------------|--------------|
| `-h` | server host                              | `127.0.0.1`  |
| `-p` | server port                              | `1234`       |
| `-c` | concurrent connections                   | `50`         |
| `-n` | total ops per phase                      | `100000`     |
| `-P` | requests in flight per conn (pipeline)   | `16`         |
| `-d` | value size (bytes) for the SET phase     | `16`         |
| `-k` | key prefix                               | `k`          |

`TCP_NODELAY` is enabled so Nagle's algorithm doesn't blunt pipelining.

### Memory methodology

`scripts/memtrack.sh` reads `VmRSS` and `VmSize` from `/proc/<pid>/status`
every 100 ms and writes a CSV (`timestamp_s,vmrss_kb,vmsize_kb`) until the
process exits or the sampler is killed.

This is deliberately simple — no `valgrind`, no `pidstat` dependency. RSS is
the right metric for "how much memory is the server actually consuming right
now": it's the resident pages backing the server's heap, stack, and code.
VmSize (virtual size) is included for completeness.

The `summary.txt` report quotes:

- `rss_min` — startup baseline before keys are inserted.
- `rss_avg` — mean across the whole run.
- `rss_peak` — maximum RSS observed (typically right after the SET phase).

A useful follow-up is `rss_peak - rss_min` divided by the number of keys, to
get a rough per-key overhead.

### Sample output

A run on a single-CPU sandbox:

```
config: host=127.0.0.1 port=1234 conns=32 ops/phase=49984 pipeline=16 value=16B prefix=k

op           ops     seconds     ops/sec    p50_us    p90_us    p99_us    max_us
SET        49984       0.362      138120     20790     21987     22135     22174
GET        49984       0.315      158898     15184     16573     16667     19534
DEL        49984       0.265      188814     14880     16243     16375     16419

-- memory (server RSS in kB, sampled every 100ms) --
samples=8  rss_min=4064  rss_avg=12591  rss_peak=14988
```

Reading that: with 32 concurrent connections and a pipeline depth of 16, the
single-threaded server services roughly 138K SET / 159K GET / 189K DEL
operations per second; p99 latency stays inside ~22 ms of total batch time
(per-op much lower); RSS grows from a 4 MB startup baseline to a ~15 MB peak
holding ~50 K small string entries.

The pattern that throughput rises SET → GET → DEL is expected: SET allocates,
GET only reads, DEL also frees. p99 latency moves the same direction.

### Reading the raw CSVs

`bench_out/bench.csv`:

```
op,ops,seconds,ops_per_sec,p50_us,p90_us,p99_us,max_us
SET,49984,0.361889,138119.67,20790,21987,22135,22174
GET,49984,0.314567,158897.97,15184,16573,16667,19534
DEL,49984,0.264727,188813.73,14880,16243,16375,16419
```

`bench_out/memtrack.csv`:

```
timestamp_s,vmrss_kb,vmsize_kb
0.000,4064,11600
0.107,12388,19920
0.213,14988,21984
...
```

These are easy to plot or load into pandas / Excel for further analysis.


## Suggested experiments

A few things the harness lets you measure cheaply:

- **Pipelining gain.** Compare `-P 1` (no pipelining) vs `-P 16` vs `-P 64`.
  Throughput should rise sharply, then plateau when the network round-trip
  stops being the bottleneck.
- **Connection scaling.** Sweep `-c 1, 4, 16, 64, 256`. On a single-threaded
  server, the curve should flatten quickly — extra connections don't add
  parallelism.
- **Per-key memory.** Run with `-n 100000` then `-n 1000000`; subtract the
  RSS baselines to back out per-key overhead.
- **Value size.** Vary `-d 16, 256, 4096`. Throughput-vs-bytes/sec turns over
  somewhere; that's where the bottleneck shifts from per-op cost to
  bandwidth.
- **DEL with large structures.** Build a big sorted set via many `zadd`s, then
  `del` it. The worker thread pool is what keeps this from stalling the
  event loop.


## Caveats

- The server uses `poll()`, not `epoll()`. That's a ceiling on connection
  count somewhere in the low thousands.
- There is no persistence, no replication, and no auth.
- Latency numbers are *per-op latency derived from pipelined batch time*, not
  the latency of an isolated request. If you need the latter, run with `-P 1`.
- A single-threaded server pinned to one core is the design — `-c` past a
  small number trades p99 for throughput, not the other way around.
