# Concurrent KV Server

A dependency-free C++17 TCP key-value server for Linux. The project demonstrates socket programming, a bounded worker pool, multithreaded synchronization, protocol design, automated testing, and reproducible performance measurement.

## Architecture

- TCP listener with configurable IPv4 bind address and port.
- Fixed-size worker pool with a bounded connection queue and backpressure.
- In-memory `std::unordered_map` protected by `std::shared_mutex`, allowing concurrent reads and exclusive writes.
- Persistent client connections with partial-read buffering and a 64 KiB request limit.
- Atomic command and connection counters.
- Graceful `SIGINT`/`SIGTERM` shutdown.

### Design tradeoffs

- A connection occupies one worker while it remains open. This keeps the implementation small and predictable, while the bounded queue prevents unlimited accepted connections from consuming memory. A production evolution would use `epoll` to multiplex many idle connections and dispatch ready commands to workers.
- `std::shared_mutex` favors the benchmark's read-heavy workload by allowing parallel GET operations. Heavy write contention would motivate sharding the map across multiple locks.
- Values are stored only in memory. Persistence, replication, authentication, and TLS are intentionally outside this project's scope.

## Protocol

Commands and responses use a newline-delimited text protocol.

| Command | Response |
|---|---|
| `PING` | `PONG` |
| `SET key value` | `STORED` |
| `GET key` | `VALUE value` or `NOT_FOUND` |
| `DEL key` | `DELETED` or `NOT_FOUND` |
| `STATS` | `STATS keys=N commands=N connections=N` |
| `QUIT` | `BYE` |

## Build and test

Requirements: Linux, GNU Make, g++ 11 or newer, and Python 3.10 or newer.

```bash
make
make test
```

The test suite includes five C++ unit tests and six integration checks, including 16 concurrent clients performing 6,400 verified SET/GET operations.

Run AddressSanitizer and UndefinedBehaviorSanitizer builds with:

```bash
make sanitize
```

## Run

```bash
./build/kv_server --host 127.0.0.1 --port 9090 --threads 8 --queue-capacity 1024
```

In another terminal:

```bash
printf 'SET course networks\nGET course\nSTATS\nQUIT\n' | nc 127.0.0.1 9090
```

The server binds to loopback by default. Use `--host 0.0.0.0` only when remote access is intentional and network controls are in place.

## Benchmark

```bash
make benchmark
```

The benchmark uses a C++ client and measures an 80% GET / 20% SET workload over persistent TCP connections. It runs five repetitions at 1, 4, 16, and 64 concurrent clients, then writes raw CSV and summarized Markdown/JSON results under `results/`.

Reported throughput and latency are local Linux loopback measurements. They are useful for reproducible comparisons and regression tracking, but should not be presented as production capacity.

### Measured results

The included run used an optimized `-O2` build, eight server workers, an 80% GET / 20% SET workload, and Linux loopback networking. Each concurrency level was repeated five times; the table reports medians.

| Clients | Requests/run | Median ops/s | Median p50 | Median p95 | Median p99 | Errors |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 3,000 | 52,617 | 15.6 us | 21.5 us | 61.2 us | 0 |
| 4 | 12,000 | 156,956 | 20.3 us | 28.1 us | 52.9 us | 0 |
| 16 | 48,000 | 188,057 | 31.8 us | 69.4 us | 112.3 us | 0 |
| 64 | 192,000 | 228,937 | 29.4 us | 62.3 us | 95.6 us | 0 |

See `results/benchmark_summary.md` for the hardware/compiler metadata and throughput ranges, and `results/benchmark_raw.csv` for every run.
