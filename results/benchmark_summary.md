# Benchmark Results

These figures are local loopback measurements, not production capacity claims.

- Generated: 2026-09-14T15:24:17.725548+00:00
- CPU: arm (8 logical CPUs visible)
- Compiler: Apple clang version 16.0.0 (clang-1600.0.26.3)
- Server workers: 8
- Workload: 80% GET / 20% SET over persistent TCP connections on Linux loopback
- Repetitions: 5 per concurrency level

| Clients | Requests/run | Median ops/s | Range ops/s | Median p50 | Median p95 | Median p99 | Errors |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 3,000 | 85,398 | 51,225-87,203 | 11.1 us | 12.9 us | 17.2 us | 0 |
| 4 | 12,000 | 265,733 | 221,076-282,164 | 13.0 us | 21.4 us | 37.7 us | 0 |
| 16 | 48,000 | 298,392 | 275,053-320,697 | 23.2 us | 30.9 us | 56.4 us | 0 |
| 64 | 192,000 | 322,693 | 298,935-324,637 | 22.8 us | 26.4 us | 36.9 us | 0 |
