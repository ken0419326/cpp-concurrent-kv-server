#!/usr/bin/env python3
import csv
import datetime as dt
import json
import os
import platform
import signal
import socket
import statistics
import subprocess
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SERVER = ROOT / "build" / "kv_server"
BENCHMARK = ROOT / "build" / "kv_benchmark"
RESULTS = ROOT / "results"
CLIENT_COUNTS = (1, 4, 16, 64)
REPETITIONS = 5
REQUESTS_PER_CLIENT = 3000
SERVER_THREADS = min(8, os.cpu_count() or 4)


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def wait_until_ready(process: subprocess.Popen[str], port: int) -> None:
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if process.poll() is not None:
            stdout, stderr = process.communicate()
            raise RuntimeError(f"server exited early: {stdout} {stderr}")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                return
        except OSError:
            time.sleep(0.02)
    raise TimeoutError("server did not become ready")


def cpu_model() -> str:
    try:
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or "unknown"


def main() -> None:
    RESULTS.mkdir(exist_ok=True)
    port = free_port()
    server = subprocess.Popen(
        [str(SERVER), "--port", str(port), "--threads", str(SERVER_THREADS), "--queue-capacity", "1024"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    rows: list[dict] = []
    try:
        wait_until_ready(server, port)
        for clients in CLIENT_COUNTS:
            for repetition in range(1, REPETITIONS + 1):
                completed = subprocess.run(
                    [
                        str(BENCHMARK), "--port", str(port),
                        "--clients", str(clients),
                        "--requests-per-client", str(REQUESTS_PER_CLIENT),
                        "--read-ratio", "80",
                        "--keyspace", "1000",
                        "--value-size", "32",
                        "--warmup", "100",
                    ],
                    check=True,
                    capture_output=True,
                    text=True,
                    timeout=180,
                )
                result = json.loads(completed.stdout.strip().splitlines()[-1])
                result["repetition"] = repetition
                rows.append(result)
                print(
                    f"clients={clients} run={repetition}/{REPETITIONS} "
                    f"throughput={result['throughput_ops_per_sec']:.0f} ops/s "
                    f"p95={result['p95_latency_us']:.1f} us"
                )
    finally:
        if server.poll() is None:
            server.send_signal(signal.SIGINT)
            try:
                server.wait(timeout=10)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait(timeout=2)

    csv_path = RESULTS / "benchmark_raw.csv"
    with csv_path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0].keys()), lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)

    summaries = []
    for clients in CLIENT_COUNTS:
        group = [row for row in rows if row["clients"] == clients]
        throughputs = [float(row["throughput_ops_per_sec"]) for row in group]
        p50s = [float(row["p50_latency_us"]) for row in group]
        p95s = [float(row["p95_latency_us"]) for row in group]
        p99s = [float(row["p99_latency_us"]) for row in group]
        summaries.append(
            {
                "clients": clients,
                "requests_per_run": int(group[0]["total_requests"]),
                "median_throughput_ops_per_sec": statistics.median(throughputs),
                "min_throughput_ops_per_sec": min(throughputs),
                "max_throughput_ops_per_sec": max(throughputs),
                "median_p50_latency_us": statistics.median(p50s),
                "median_p95_latency_us": statistics.median(p95s),
                "median_p99_latency_us": statistics.median(p99s),
                "errors": sum(int(row["errors"]) for row in group),
            }
        )

    metadata = {
        "generated_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "os": platform.platform(),
        "cpu_model": cpu_model(),
        "logical_cpus_visible": os.cpu_count(),
        "compiler": subprocess.run(["g++", "--version"], capture_output=True, text=True, check=True).stdout.splitlines()[0],
        "server_threads": SERVER_THREADS,
        "repetitions": REPETITIONS,
        "workload": "80% GET / 20% SET over persistent TCP connections on Linux loopback",
        "summaries": summaries,
    }
    (RESULTS / "benchmark_summary.json").write_text(json.dumps(metadata, indent=2) + "\n")

    markdown = [
        "# Benchmark Results",
        "",
        "These figures are local loopback measurements, not production capacity claims.",
        "",
        f"- Generated: {metadata['generated_at_utc']}",
        f"- CPU: {metadata['cpu_model']} ({metadata['logical_cpus_visible']} logical CPUs visible)",
        f"- Compiler: {metadata['compiler']}",
        f"- Server workers: {SERVER_THREADS}",
        f"- Workload: {metadata['workload']}",
        f"- Repetitions: {REPETITIONS} per concurrency level",
        "",
        "| Clients | Requests/run | Median ops/s | Range ops/s | Median p50 | Median p95 | Median p99 | Errors |",
        "|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for row in summaries:
        markdown.append(
            f"| {row['clients']} | {row['requests_per_run']:,} | "
            f"{row['median_throughput_ops_per_sec']:,.0f} | "
            f"{row['min_throughput_ops_per_sec']:,.0f}-{row['max_throughput_ops_per_sec']:,.0f} | "
            f"{row['median_p50_latency_us']:.1f} us | "
            f"{row['median_p95_latency_us']:.1f} us | "
            f"{row['median_p99_latency_us']:.1f} us | {row['errors']} |"
        )
    (RESULTS / "benchmark_summary.md").write_text("\n".join(markdown) + "\n")
    print(f"wrote {csv_path}")
    print(f"wrote {RESULTS / 'benchmark_summary.md'}")


if __name__ == "__main__":
    main()
