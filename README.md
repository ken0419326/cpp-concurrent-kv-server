# Concurrent KV Server: worker pool and epoll

A dependency-free C++17 TCP key-value server with two Linux implementations of the same protocol: the original connection-per-worker baseline and a single-threaded, level-triggered epoll server.

## Architectures and request flow

| | `build/kv_server` baseline | `build/kv_server_epoll` |
|---|---|---|
| Concurrency | Main thread accepts; fixed worker pool handles connections | One event-loop thread handles all ready sockets |
| Idle connection | Occupies a worker blocked in `recv()` | Retains connection state and fd, but no thread waits on it |
| Queue | Bounded queue of accepted connections; backpressure on `submit()` | No worker queue; epoll interest list tracks sockets |
| I/O | Blocking client `recv()` and `send()` | Non-blocking `accept4()`, `recv()`, and `send()` |
| Store | `KvStore` with `std::shared_mutex` | The same `KvStore`, called from one event-loop thread |

Baseline flow: `poll(listener)` → `accept()` → bounded `ThreadPool::submit()` → worker `handle_client()` → buffer until newline → `parse_command()` → `KvStore` → `send_all()`. One persistent connection can send many commands, but an idle one still holds its worker.

epoll flow: `epoll_wait()` → drain `accept4()` until `EAGAIN` → register non-blocking client → drain readable bytes until `EAGAIN` → parse complete lines with the same `parse_command()` → update the same `KvStore` → queue responses per connection → send until complete or `EAGAIN`; subscribe to `EPOLLOUT` only while bytes remain. `QUIT` queues `BYE` and closes after its complete write. epoll is readiness notification, **not multithreading**.

The epoll loop uses level-triggered readiness because any still-readable or still-writable fd will be reported again, which simplifies this learning implementation. Each event carries a monotonic connection token rather than an fd; a stale event cannot refer to a newly reused fd.

### Design tradeoffs and limits

- The epoll server has one event-loop thread. CPU-heavy command work can delay all clients; it is not a multi-core processing design.
- The baseline can stall graceful shutdown while an idle persistent client remains blocked in `recv()`; the epoll server closes open client fds when stopping.
- Both use one in-memory map. There is no persistence, replication, authentication, or TLS. Neither server is a production database.
- epoll is Linux-only. The baseline and benchmark client also compile on macOS, but native macOS cannot build the epoll target.
- The epoll server currently has no pending-output memory cap; an unbounded pipelined response burst can consume memory.

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

Requirements for both binaries: Linux, GNU Make, g++ 11 or newer, and Python 3.10 or newer. Build flags include `-Wall -Wextra -Wpedantic -Werror`.

```bash
make
make test
make sanitize
```

`make test` runs five C++ unit tests and the same integration suite against each server, including fragmentation, pipelining, large queued responses, 16 concurrent clients, idle clients, `QUIT`, disconnects, oversize requests, fd churn, and signal shutdown. The epoll-only idle test checks that 64 idle clients do not block an active one. `make sanitize` rebuilds and runs AddressSanitizer and UndefinedBehaviorSanitizer checks. ThreadSanitizer is optional and must be reported separately.

On macOS, `make` and `make test` cover only the baseline. To compile and verify the Linux epoll binary in Docker Desktop:

```bash
sh scripts/docker_verify.sh
```

This script runs `make test` and `make sanitize` inside a Debian container. A running Docker daemon is required. Docker Desktop performance figures are **containerized development measurements**, not native Linux results for a résumé.

## Run

```bash
./build/kv_server --host 127.0.0.1 --port 9090 --threads 8 --queue-capacity 1024
# Or, on Linux:
./build/kv_server_epoll --host 127.0.0.1 --port 9090
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

This legacy command runs only the baseline and overwrites the original `results/benchmark_*` files. For a paired before/after comparison on a native Linux host, run:

```bash
make clean
make
make test
make sanitize
make clean
make
make benchmark-compare
```

`make benchmark-compare` writes **new** `results/epoll_comparison_raw.csv`, `results/epoll_comparison_summary.json`, and `results/epoll_comparison_summary.md`; it never overwrites the legacy files. It starts a fresh server for each run, uses the same compiler flags and `build/kv_benchmark` for both, alternates server order, and measures 1, 8, 64, and 256 persistent clients five times each. Both use Linux loopback, 80% GET/20% SET, keyspace 1000, 32-byte values, 100 warm-up PINGs, and 3000 measured requests per client. The baseline has eight workers; the epoll server has one event-loop thread. Concurrent clients are **not** worker threads. Raw CSV retains failures and timeouts; incomplete groups have no median or claimed improvement.

The script reports median throughput and range, median p50/p95/p99 latency, errors, and completed requests. Throughput change is `(epoll median − baseline median) / baseline median × 100`. Latency reduction is `(baseline median − epoll median) / baseline median × 100`, where positive means lower epoll latency. It also probes 0, 8, 64, 256, and 1024 idle TCP connections, measures active PING timeout, and reads server RSS and fd count from `/proc`; settings beyond the fd soft limit are marked skipped.

For a Docker Desktop development run, build the image and run the same comparison explicitly, labeling its output as containerized:

```bash
docker build -t cpp-kv-epoll-verify .
docker run --rm --mount type=bind,src="$PWD/results",dst=/work/results cpp-kv-epoll-verify sh -c 'make && make benchmark-compare'
```

Loopback throughput is not production capacity. Compare both servers on the **same** native Linux machine and repeat under different loads before drawing architectural conclusions.

### Before/after comparison

The following paired run was measured in a **Docker Desktop Linux container on an ARM64 Mac**, using Debian g++ 12.2, `-O2 -Wall -Wextra -Wpedantic -Werror -pthread`. It is a containerized development measurement, **not** a native Linux or production-capacity result. Each row is five repetitions per server; all requests completed with zero errors. Negative throughput change means the epoll version was slower here.

| Clients | Baseline median ops/s (range) | epoll median ops/s (range) | Change | p95 baseline → epoll | Errors B/E | Completed B/E |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 32,520 (23,410–32,931) | 31,716 (31,303–31,807) | −2.5% | 31.5 → 32.8 µs | 0/0 | 15,000/15,000 |
| 8 | 375,711 (328,106–400,163) | 130,704 (125,498–136,405) | −65.2% | 72.9 → 94.1 µs | 0/0 | 120,000/120,000 |
| 64 | 140,389 (130,543–159,752) | 131,926 (127,495–134,681) | −6.0% | 97.0 → 504.1 µs | 0/0 | 960,000/960,000 |
| 256 | 141,483 (140,792–153,371) | 129,788 (126,958–131,375) | −8.3% | 79.9 → 2,039.4 µs | 0/0 | 3,840,000/3,840,000 |

With eight idle connections, baseline active PING timed out at one second; epoll replied in 0.847 ms. At 64 and 256 idle connections the baseline also timed out while epoll replied. At a target of 1024, baseline opened 573 before a connection timeout, while epoll opened 1024 and replied in 0.586 ms. The full idle table includes `/proc` RSS and server fd counts in `results/epoll_comparison_summary.md`.

### Measured results

The legacy `results/benchmark_*` files are **baseline-only** and cannot serve as the before half of a paired comparison. Their JSON metadata identifies macOS, although the older README called them Linux loopback; that earlier platform label was incorrect. The table below preserves those historical baseline-only numbers, not a Linux before/after result.

| Clients | Requests/run | Median ops/s | Median p50 | Median p95 | Median p99 | Errors |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 3,000 | 85,398 | 11.1 us | 12.9 us | 17.2 us | 0 |
| 4 | 12,000 | 265,733 | 13.0 us | 21.4 us | 37.7 us | 0 |
| 16 | 48,000 | 298,392 | 23.2 us | 30.9 us | 56.4 us | 0 |
| 64 | 192,000 | 322,693 | 23.2 us | 26.4 us | 36.9 us | 0 |

See `results/benchmark_summary.md` for the hardware/compiler metadata and throughput ranges, and `results/benchmark_raw.csv` for every run.
