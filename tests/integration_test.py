#!/usr/bin/env python3
import concurrent.futures
import contextlib
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SERVER = ROOT / "build" / "kv_server"


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


class Client:
    def __init__(self, port: int):
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=3)
        self.stream = self.socket.makefile("rwb", buffering=0)

    def request(self, command: str) -> str:
        self.stream.write((command + "\n").encode())
        response = self.stream.readline()
        if not response:
            raise RuntimeError("server closed connection")
        return response.decode().rstrip("\r\n")

    def close(self) -> None:
        with contextlib.suppress(Exception):
            self.stream.close()
        with contextlib.suppress(Exception):
            self.socket.close()


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


def concurrency_worker(port: int, worker_id: int) -> None:
    client = Client(port)
    try:
        for i in range(200):
            key = f"worker:{worker_id}:{i}"
            assert client.request(f"SET {key} value") == "STORED"
            assert client.request(f"GET {key}") == "VALUE value"
    finally:
        client.close()


def main() -> int:
    port = free_port()
    process = subprocess.Popen(
        [str(SERVER), "--port", str(port), "--threads", "16", "--queue-capacity", "128"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    passed = 0
    try:
        wait_until_ready(process, port)
        client = Client(port)
        try:
            assert client.request("PING") == "PONG"
            passed += 1
            assert client.request("SET greeting hello world") == "STORED"
            assert client.request("GET greeting") == "VALUE hello world"
            passed += 1
            assert client.request("DEL greeting") == "DELETED"
            assert client.request("GET greeting") == "NOT_FOUND"
            passed += 1
            assert client.request("GET") == "ERROR GET requires a key"
            passed += 1
            assert client.request("STATS").startswith("STATS keys=")
            passed += 1
        finally:
            client.close()

        with concurrent.futures.ThreadPoolExecutor(max_workers=16) as executor:
            futures = [executor.submit(concurrency_worker, port, i) for i in range(16)]
            for future in futures:
                future.result()
        passed += 1
        print(f"integration_tests_passed={passed}")
        return 0
    finally:
        if process.poll() is None:
            process.send_signal(signal.SIGINT)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=2)


if __name__ == "__main__":
    sys.exit(main())
