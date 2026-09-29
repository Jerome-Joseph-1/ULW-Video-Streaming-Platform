#!/usr/bin/env python3
"""Opens many short-lived connections against the gateway as fast as this process can, mixing
three request shapes, and reports what came back plus — as slowloris.py does — a concurrent
legitimate client's latency and error rate so a run can show it held up under the flood.

  unauth   GET /api/v1/videos/{random-uuid} with no Authorization header; requires_auth in
           apps/gateway/src/routes.hpp says this route needs a token, so every one of these
           should get a fast 401 without touching the catalog.
  bad      a request the HTTP layer itself should refuse before routing: a header line over the
           parser's limit, run once per connection and then the socket is dropped — this checks
           that malformed input is turned away cheaply rather than accepted onto a route.
  valid    (only if --token is given) GET /api/v1/healthz with a valid bearer token, the
           cheapest authenticated route, so a flood run can also show real traffic keeps working.

    tests/load/flood.py --url http://127.0.0.1:8080 --duration 30 --rate 500 \
        --legit-url http://127.0.0.1:8080 --token "$TOKEN"

--rate bounds requests/s across all worker threads combined (0 means unbounded, limited only by
--workers). Each connection is opened, one request sent, response drained, then closed — no
keep-alive — since the point is connection churn, not throughput per connection.
"""
import argparse
import http.client
import json
import socket
import statistics
import threading
import time
import urllib.parse
import uuid


class Flood:
    def __init__(self):
        self.lock = threading.Lock()
        self.sent = 0
        self.status_counts = {}
        self.errors = {}

    def record(self, status=None, error=None):
        with self.lock:
            self.sent += 1
            if status is not None:
                self.status_counts[status] = self.status_counts.get(status, 0) + 1
            if error is not None:
                self.errors[error] = self.errors.get(error, 0) + 1

    def to_dict(self):
        with self.lock:
            return {
                "sent": self.sent,
                "status_counts": dict(self.status_counts),
                "errors_by_kind": dict(self.errors),
            }


def send_unauth(host, port):
    parts_path = f"/api/v1/videos/{uuid.uuid4()}"
    conn = http.client.HTTPConnection(host, port, timeout=5)
    try:
        conn.request("GET", parts_path)
        resp = conn.getresponse()
        resp.read()
        return resp.status
    finally:
        conn.close()


def send_bad(host, port):
    """A request line the parser should refuse outright: a header name/value pair far past any
    reasonable size, sent raw so nothing in http.client normalizes it away first."""
    sock = socket.create_connection((host, port), timeout=5)
    try:
        oversized = "X-Flood: " + ("a" * 200_000) + "\r\n"
        request = f"GET /api/v1/healthz HTTP/1.1\r\nHost: load-test\r\n{oversized}\r\n"
        sock.sendall(request.encode())
        sock.settimeout(5)
        data = sock.recv(4096)
        if not data:
            return "closed"
        line = data.split(b"\r\n", 1)[0].decode(errors="replace")
        # First token after the HTTP version is the status code, e.g. "HTTP/1.1 400 ...".
        parts = line.split()
        return int(parts[1]) if len(parts) > 1 and parts[1].isdigit() else line
    finally:
        sock.close()


def send_valid(host, port, token):
    conn = http.client.HTTPConnection(host, port, timeout=5)
    try:
        conn.request("GET", "/api/v1/healthz", headers={"Authorization": f"Bearer {token}"})
        resp = conn.getresponse()
        resp.read()
        return resp.status
    finally:
        conn.close()


def worker(host, port, token, deadline, rate_gate, flood):
    # Cycle through the shapes so each worker contributes all three kinds rather than one
    # worker per kind, which would skew status counts toward whichever kind has fewer workers
    # when --workers doesn't divide evenly by the number of kinds.
    shapes = [("unauth", lambda: send_unauth(host, port)), ("bad", lambda: send_bad(host, port))]
    if token:
        shapes.append(("valid", lambda: send_valid(host, port, token)))
    while time.monotonic() < deadline:
        for kind, fn in shapes:
            if time.monotonic() >= deadline:
                return
            if rate_gate is not None:
                rate_gate()
            try:
                result = fn()
            except OSError as e:
                flood.record(error=f"{kind}:{type(e).__name__}")
                continue
            flood.record(status=f"{kind}:{result}")


class RateGate:
    """Caps combined requests/s across every worker thread by handing out send slots on a fixed
    schedule, not by sleeping a guessed amount per worker — accurate regardless of --workers."""

    def __init__(self, rate):
        self.interval = 1.0 / rate
        self.lock = threading.Lock()
        self.next_slot = time.monotonic()

    def __call__(self):
        with self.lock:
            now = time.monotonic()
            if self.next_slot < now:
                self.next_slot = now
            wait = self.next_slot - now
            self.next_slot += self.interval
        if wait > 0:
            time.sleep(wait)


def run_legit_client(url, token, duration):
    stop_event = threading.Event()
    latencies = []
    errors = []
    lock = threading.Lock()

    def loop():
        parts = urllib.parse.urlsplit(url)
        kind = http.client.HTTPSConnection if parts.scheme == "https" else http.client.HTTPConnection
        while not stop_event.is_set():
            started = time.monotonic()
            try:
                conn = kind(parts.hostname, parts.port, timeout=10)
                headers = {"Authorization": f"Bearer {token}"} if token else {}
                conn.request("GET", "/api/v1/healthz", headers=headers)
                resp = conn.getresponse()
                resp.read()
                conn.close()
                elapsed = time.monotonic() - started
                with lock:
                    latencies.append(elapsed)
                    if resp.status not in (200, 204):
                        errors.append(f"http_{resp.status}")
            except OSError as e:
                with lock:
                    errors.append(type(e).__name__)
            stop_event.wait(0.2)

    thread = threading.Thread(target=loop, daemon=True)
    thread.start()

    def stop():
        stop_event.set()
        thread.join(timeout=10)
        with lock:
            data = sorted(latencies)
            err = list(errors)
        summary = {
            "requests": len(data) + len(err),
            "errors": len(err),
            "error_rate": round(len(err) / (len(data) + len(err)), 4) if (data or err) else None,
        }
        if data:
            qs = statistics.quantiles(data, n=100) if len(data) > 1 else [data[0]] * 99
            summary["p50_ms"] = round((qs[49] if len(data) > 1 else data[0]) * 1000, 2)
            summary["p99_ms"] = round((qs[98] if len(data) > 1 else data[0]) * 1000, 2)
        return summary

    return stop


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--url", required=True)
    parser.add_argument("--duration", type=float, default=30.0)
    parser.add_argument("--workers", type=int, default=64)
    parser.add_argument("--rate", type=float, default=0,
                        help="combined requests/s across all workers; 0 means unbounded")
    parser.add_argument("--token", help="bearer token; adds the 'valid' request shape when given")
    parser.add_argument("--legit-url", help="base URL for a concurrent legit client loop")
    parser.add_argument("--legit-token", help="bearer token for the legit client, if it needs one")
    args = parser.parse_args()

    parts = urllib.parse.urlsplit(args.url)
    flood = Flood()
    deadline = time.monotonic() + args.duration
    rate_gate = RateGate(args.rate) if args.rate > 0 else None

    stop_legit = None
    if args.legit_url:
        stop_legit = run_legit_client(args.legit_url, args.legit_token, args.duration)

    started = time.monotonic()
    threads = [threading.Thread(target=worker,
                                args=(parts.hostname, parts.port, args.token, deadline, rate_gate,
                                      flood))
              for _ in range(args.workers)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    wall = time.monotonic() - started

    report = flood.to_dict()
    report["wall_time_s"] = round(wall, 3)
    report["requests_per_second"] = round(report["sent"] / wall, 2) if wall > 0 else None
    if stop_legit:
        report["legit_client"] = stop_legit()
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
