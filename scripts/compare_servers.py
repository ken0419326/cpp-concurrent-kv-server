#!/usr/bin/env python3
"""Paired Linux-loopback benchmark; retain every failure and timeout."""
import argparse
import csv
import datetime as dt
import json
import os
import platform
import resource
import signal
import socket
import statistics
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RESULTS = ROOT / "results"
SERVERS = {"baseline": ROOT / "build/kv_server", "epoll": ROOT / "build/kv_server_epoll"}
BENCHMARK = ROOT / "build/kv_benchmark"
CLIENTS = (1, 8, 64, 256)
REPETITIONS = 5
REQUESTS = 3000
WORKERS = 8


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def start_server(kind):
    port = free_port()
    cmd = [str(SERVERS[kind]), "--port", str(port)]
    if kind == "baseline":
        cmd += ["--threads", str(WORKERS), "--queue-capacity", "1024"]
    process = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"{kind} exited during startup: {process.stderr.read()}")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                return process, port
        except OSError:
            time.sleep(0.02)
    process.kill()
    process.wait()
    raise TimeoutError(f"{kind} startup timed out")


def stop_server(process):
    if process.poll() is None:
        process.send_signal(signal.SIGINT)
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=2)
            return "forced_kill"
    return f"exit_{process.returncode}"


def run_workload(kind, clients, repetition):
    row = {"kind": kind, "clients": clients, "repetition": repetition, "status": "failed",
           "errors": None, "completed_requests": None, "throughput_ops_per_sec": None,
           "p50_latency_us": None, "p95_latency_us": None, "p99_latency_us": None}
    process = None
    try:
        process, port = start_server(kind)
        cmd = [str(BENCHMARK), "--port", str(port), "--clients", str(clients),
               "--requests-per-client", str(REQUESTS), "--read-ratio", "80",
               "--keyspace", "1000", "--value-size", "32", "--warmup", "100"]
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
        if result.stdout.strip():
            data = json.loads(result.stdout.strip().splitlines()[-1])
            row.update(errors=data["errors"], completed_requests=data["total_requests"],
                       throughput_ops_per_sec=data["throughput_ops_per_sec"],
                       p50_latency_us=data["p50_latency_us"],
                       p95_latency_us=data["p95_latency_us"], p99_latency_us=data["p99_latency_us"])
        row["status"] = "ok" if result.returncode == 0 and row["errors"] == 0 and row["completed_requests"] == clients * REQUESTS else f"failed_exit_{result.returncode}"
        if row["status"] != "ok":
            row["detail"] = result.stderr[-500:]
    except subprocess.TimeoutExpired:
        row["status"] = "timeout"
    except Exception as error:
        row["status"] = "failed"
        row["detail"] = str(error)
    finally:
        if process is not None:
            row["server_shutdown"] = stop_server(process)
    return row


def proc_metrics(pid):
    status = (Path("/proc") / str(pid) / "status").read_text()
    rss = next(int(line.split()[1]) for line in status.splitlines() if line.startswith("VmRSS:"))
    fds = len(list((Path("/proc") / str(pid) / "fd").iterdir()))
    return rss, fds


def idle_run(kind, target, fd_limit):
    row = {"kind": kind, "target_idle": target, "opened_idle": 0, "status": "failed",
           "ping_ms": None, "server_rss_kib": None, "server_open_fds": None,
           "fd_soft_limit": fd_limit}
    if target + 64 >= fd_limit:
        row["status"] = "skipped_fd_limit"
        return row
    process = None
    sockets = []
    try:
        process, port = start_server(kind)
        for _ in range(target):
            sock = socket.create_connection(("127.0.0.1", port), timeout=1)
            sockets.append(sock)
            row["opened_idle"] += 1
        time.sleep(0.2)
        row["server_rss_kib"], row["server_open_fds"] = proc_metrics(process.pid)
        started = time.monotonic()
        with socket.create_connection(("127.0.0.1", port), timeout=1) as active:
            active.settimeout(1)
            active.sendall(b"PING\n")
            response = active.recv(64)
        row["ping_ms"] = round((time.monotonic() - started) * 1000, 3)
        row["status"] = "ok" if response == b"PONG\n" else "wrong_response"
    except socket.timeout:
        row["status"] = "ping_timeout" if row["opened_idle"] == target else "connect_timeout"
    except OSError as error:
        row["status"] = "socket_limit_or_error"
        row["detail"] = str(error)
    except Exception as error:
        row["detail"] = str(error)
    finally:
        for sock in sockets:
            sock.close()
        if process is not None:
            row["server_shutdown"] = stop_server(process)
    return row


def summarize(rows):
    output = []
    for clients in CLIENTS:
        pair = {"clients": clients, "runs": {}}
        for kind in SERVERS:
            group = [row for row in rows if row["kind"] == kind and row["clients"] == clients]
            good = len(group) == REPETITIONS and all(row["status"] == "ok" for row in group)
            item = {"complete": good, "statuses": [row["status"] for row in group],
                    "errors": sum(row["errors"] or 0 for row in group),
                    "completed_requests": sum(row["completed_requests"] or 0 for row in group)}
            if good:
                throughputs = [row["throughput_ops_per_sec"] for row in group]
                item.update(median_throughput=statistics.median(throughputs),
                            throughput_range=[min(throughputs), max(throughputs)])
                for percentile in ("p50", "p95", "p99"):
                    item[f"median_{percentile}_us"] = statistics.median(row[f"{percentile}_latency_us"] for row in group)
            pair["runs"][kind] = item
        base, epoll = pair["runs"]["baseline"], pair["runs"]["epoll"]
        if base["complete"] and epoll["complete"]:
            pair["throughput_improvement_percent"] = (epoll["median_throughput"] - base["median_throughput"]) / base["median_throughput"] * 100
            pair["p95_latency_reduction_percent"] = (base["median_p95_us"] - epoll["median_p95_us"]) / base["median_p95_us"] * 100
        output.append(pair)
    return output


def write_results(rows, summary):
    RESULTS.mkdir(exist_ok=True)
    columns = ["kind", "clients", "repetition", "status", "errors", "completed_requests",
               "throughput_ops_per_sec", "p50_latency_us", "p95_latency_us", "p99_latency_us",
               "server_shutdown", "detail"]
    with (RESULTS / "epoll_comparison_raw.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=columns)
        writer.writeheader()
        writer.writerows(rows)
    (RESULTS / "epoll_comparison_summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    lines = ["# Baseline versus single-threaded epoll", "", summary["measurement_label"], "",
             f"OS: {summary['os']}; CPU: {summary['cpu']}; compiler: {summary['compiler']}",
             f"Flags: {summary['compiler_flags']}; 80% GET / 20% SET, keyspace 1000, value 32 bytes, warm-up 100, 3000 requests/client, five repetitions.",
             "Fresh server for every run; baseline has 8 workers. Failed or timed-out runs remain in raw CSV and suppress medians.", "",
             "| Clients | Baseline median ops/s (range) | epoll median ops/s (range) | Throughput change | Baseline p50/p95/p99 us | epoll p50/p95/p99 us | p95 reduction | Errors B/E | Completed B/E |",
             "|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for group in summary["workload"]:
        b, e = group["runs"]["baseline"], group["runs"]["epoll"]
        def cells(item):
            if not item["complete"]:
                return "INCOMPLETE", "INCOMPLETE"
            return (f"{item['median_throughput']:,.0f} ({item['throughput_range'][0]:,.0f}–{item['throughput_range'][1]:,.0f})",
                    f"{item['median_p50_us']:.1f}/{item['median_p95_us']:.1f}/{item['median_p99_us']:.1f}")
        bt, bl = cells(b)
        et, el = cells(e)
        change = f"{group['throughput_improvement_percent']:+.1f}%" if "throughput_improvement_percent" in group else "N/A"
        reduction = f"{group['p95_latency_reduction_percent']:+.1f}%" if "p95_latency_reduction_percent" in group else "N/A"
        lines.append(f"| {group['clients']} | {bt} | {et} | {change} | {bl} | {el} | {reduction} | {b['errors']}/{e['errors']} | {b['completed_requests']}/{e['completed_requests']} |")
    lines += ["", "Latency reduction = (baseline median − epoll median) / baseline median × 100; positive means lower epoll latency.",
              "Throughput change = (epoll median − baseline median) / baseline median × 100.", "", "## Idle connections", "",
              "| Idle target | Server | Opened | Ping | RSS KiB | Server FDs | Status |",
              "|---:|---|---:|---:|---:|---:|---|"]
    for row in summary["idle_connections"]:
        ping = f"{row['ping_ms']} ms" if row["ping_ms"] is not None else "—"
        rss = row["server_rss_kib"] if row["server_rss_kib"] is not None else "—"
        fds = row["server_open_fds"] if row["server_open_fds"] is not None else "—"
        lines.append(f"| {row['target_idle']} | {row['kind']} | {row['opened_idle']} | {ping} | {rss} | {fds} | {row['status']} |")
    (RESULTS / "epoll_comparison_summary.md").write_text("\n".join(lines) + "\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--skip-workload", action="store_true")
    args = parser.parse_args()
    if platform.system() != "Linux":
        raise SystemExit("Linux required; Docker Desktop measurements must be labeled separately")
    flags = os.environ.get("BENCHMARK_CXXFLAGS", "unknown (build flags not supplied)")
    compiler = subprocess.run([os.environ.get("CXX", "g++"), "--version"], capture_output=True, text=True, check=True).stdout.splitlines()[0]
    label = "containerized development measurement" if Path("/.dockerenv").exists() else "native Linux loopback measurement (not production capacity)"
    rows = []
    if not args.skip_workload:
        for clients in CLIENTS:
            for repetition in range(1, REPETITIONS + 1):
                for kind in ("baseline", "epoll") if repetition % 2 else ("epoll", "baseline"):
                    row = run_workload(kind, clients, repetition)
                    rows.append(row)
                    print(kind, clients, repetition, row["status"], flush=True)
    soft, _ = resource.getrlimit(resource.RLIMIT_NOFILE)
    idle = []
    for target in (0, 8, 64, 256, 1024):
        for kind in SERVERS:
            row = idle_run(kind, target, soft)
            idle.append(row)
            print("idle", kind, target, row["status"], flush=True)
    summary = {"generated_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
               "measurement_label": label, "os": platform.platform(), "cpu": platform.processor() or platform.machine(),
               "logical_cpus": os.cpu_count(), "compiler": compiler, "compiler_flags": flags,
               "client_binary": str(BENCHMARK), "baseline_workers": WORKERS,
               "requests_per_client": REQUESTS, "repetitions": REPETITIONS,
               "workload": summarize(rows), "idle_connections": idle}
    write_results(rows, summary)
    if any(row["status"] != "ok" for row in rows):
        raise SystemExit("some workload runs failed; see raw CSV")


if __name__ == "__main__":
    main()
