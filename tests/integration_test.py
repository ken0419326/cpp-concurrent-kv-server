#!/usr/bin/env python3
"""Run the same TCP contract checks against either server."""
import argparse
import concurrent.futures
import signal
import socket
import subprocess
import time
import weakref
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RECEIVE_BUFFERS = weakref.WeakKeyDictionary()


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def connect(port):
    sock = socket.create_connection(("127.0.0.1", port), timeout=5)
    sock.settimeout(5)
    return sock


def line(sock):
    data = RECEIVE_BUFFERS.get(sock, b"")
    while b"\n" not in data:
        chunk = sock.recv(8192)
        assert chunk, "connection closed before response"
        data += chunk
    current, remaining = data.split(b"\n", 1)
    RECEIVE_BUFFERS[sock] = remaining
    return current.decode().rstrip("\r")


def request(sock, command):
    sock.sendall((command + "\n").encode())
    return line(sock)


def wait_ready(process, port):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"server exited early: {process.communicate()}")
        try:
            with connect(port):
                return
        except OSError:
            time.sleep(0.02)
    raise TimeoutError("server did not become ready")


def concurrent_worker(port, worker_id):
    with connect(port) as sock:
        for i in range(200):
            key = f"worker:{worker_id}:{i}"
            assert request(sock, f"SET {key} value") == "STORED"
            assert request(sock, f"GET {key}") == "VALUE value"


def run_checks(port, epoll):
    passed = []
    with connect(port) as sock:
        assert request(sock, "PING") == "PONG"
        assert request(sock, "SET greeting hello world") == "STORED"
        assert request(sock, "GET greeting") == "VALUE hello world"
        assert request(sock, "DEL greeting") == "DELETED"
        assert request(sock, "GET greeting") == "NOT_FOUND"
        assert request(sock, "GET") == "ERROR GET requires a key"
        assert request(sock, "STATS").startswith("STATS keys=")
        passed.append("protocol")

        for piece in (b"SET frag", b"mented va", b"lue\n"):
            sock.sendall(piece)
        assert line(sock) == "STORED"
        assert request(sock, "GET fragmented") == "VALUE value"
        passed.append("fragmented_request")

        sock.sendall(b"PING\nSET pipe one\nGET pipe\n")
        assert [line(sock) for _ in range(3)] == ["PONG", "STORED", "VALUE one"]
        passed.append("pipelined_requests")

        value = "x" * 4096
        assert request(sock, f"SET large {value}") == "STORED"
        count = 1024
        sock.sendall(b"GET large\n" * count)
        time.sleep(0.05)  # Let the server fill its send buffer before reading.
        assert all(line(sock) == f"VALUE {value}" for _ in range(count))
        passed.append("queued_large_responses")

        assert request(sock, "QUIT") == "BYE"
        assert sock.recv(1) == b""
        passed.append("quit_after_bye")

    with concurrent.futures.ThreadPoolExecutor(max_workers=16) as executor:
        list(executor.map(lambda i: concurrent_worker(port, i), range(16)))
    passed.append("concurrent_clients")

    with connect(port) as sock:
        sock.sendall(b"PING\n")
    with connect(port) as sock:
        assert request(sock, "PING") == "PONG"
    passed.append("abrupt_disconnect")

    with connect(port) as sock:
        sock.sendall(b"SET huge " + b"x" * (64 * 1024 + 1))
        assert line(sock) == "ERROR request too large"
        assert sock.recv(1) == b""
    passed.append("oversize_request")

    for _ in range(400):
        with connect(port) as sock:
            assert request(sock, "PING") == "PONG"
    passed.append("fd_reuse_churn")

    if epoll:
        idle = [connect(port) for _ in range(64)]
        try:
            with connect(port) as active:
                assert request(active, "PING") == "PONG"
        finally:
            for sock in idle:
                sock.close()
        passed.append("idle_clients_do_not_block_active")
    return passed


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", type=Path, default=ROOT / "build" / "kv_server")
    args = parser.parse_args()
    epoll = args.server.name.endswith("_epoll")
    port = free_port()
    command = [str(args.server.resolve()), "--port", str(port)]
    if not epoll:
        command += ["--threads", "16", "--queue-capacity", "128"]
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        wait_ready(process, port)
        passed = run_checks(port, epoll)
        process.send_signal(signal.SIGINT)
        stdout, stderr = process.communicate(timeout=8)
        assert process.returncode == 0, (stdout, stderr)
        passed.append("graceful_shutdown")
        for name in passed:
            print(f"PASS {args.server.name} {name}")
        print(f"integration_tests_passed={len(passed)} server={args.server.name}")
        return 0
    finally:
        if process.poll() is None:
            process.kill()
            process.communicate(timeout=2)


if __name__ == "__main__":
    raise SystemExit(main())
