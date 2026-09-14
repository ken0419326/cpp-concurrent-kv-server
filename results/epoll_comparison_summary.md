# Baseline versus single-threaded epoll

containerized development measurement

OS: Linux-6.10.14-linuxkit-aarch64-with-glibc2.36; CPU: aarch64; compiler: g++ (Debian 12.2.0-14+deb12u1) 12.2.0
Flags: -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -pthread; 80% GET / 20% SET, keyspace 1000, value 32 bytes, warm-up 100, 3000 requests/client, five repetitions.
Fresh server for every run; baseline has 8 workers. Failed or timed-out runs remain in raw CSV and suppress medians.

| Clients | Baseline median ops/s (range) | epoll median ops/s (range) | Throughput change | Baseline p50/p95/p99 us | epoll p50/p95/p99 us | p95 reduction | Errors B/E | Completed B/E |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 32,520 (23,410–32,931) | 31,716 (31,303–31,807) | -2.5% | 29.3/31.5/35.0 | 30.2/32.8/36.9 | -4.1% | 0/0 | 15000/15000 |
| 8 | 375,711 (328,106–400,163) | 130,704 (125,498–136,405) | -65.2% | 5.6/72.9/140.4 | 47.2/94.1/106.3 | -29.2% | 0/0 | 120000/120000 |
| 64 | 140,389 (130,543–159,752) | 131,926 (127,495–134,681) | -6.0% | 52.0/97.0/141.3 | 464.1/504.1/934.2 | -419.4% | 0/0 | 960000/960000 |
| 256 | 141,483 (140,792–153,371) | 129,788 (126,958–131,375) | -8.3% | 53.5/79.9/113.7 | 1961.4/2039.4/2364.7 | -2451.9% | 0/0 | 3840000/3840000 |

Latency reduction = (baseline median − epoll median) / baseline median × 100; positive means lower epoll latency.
Throughput change = (epoll median − baseline median) / baseline median × 100.

## Idle connections

| Idle target | Server | Opened | Ping | RSS KiB | Server FDs | Status |
|---:|---|---:|---:|---:|---:|---|
| 0 | baseline | 0 | 0.645 ms | 2820 | 4 | ok |
| 0 | epoll | 0 | 0.462 ms | 2816 | 5 | ok |
| 8 | baseline | 8 | — | 2820 | 12 | ping_timeout |
| 8 | epoll | 8 | 0.847 ms | 2816 | 13 | ok |
| 64 | baseline | 64 | — | 2820 | 68 | ping_timeout |
| 64 | epoll | 64 | 0.685 ms | 2804 | 69 | ok |
| 256 | baseline | 256 | — | 2812 | 260 | ping_timeout |
| 256 | epoll | 256 | 0.967 ms | 2816 | 261 | ok |
| 1024 | baseline | 573 | — | — | — | connect_timeout |
| 1024 | epoll | 1024 | 0.586 ms | 2816 | 1029 | ok |
