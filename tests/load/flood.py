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
  valid    (only if --token is given) GET /api/v1/videos/{random-uuid} with a valid bearer token:
           an authenticated catalog lookup that answers 404, so the flood also reaches the
           authenticated path.

    tests/load/flood.py --url http://127.0.0.1:8080 --duration 30 --rate 500 \
        --legit-url http://127.0.0.1:8080 --token "$TOKEN"

--legit-url with --legit-token runs the client in legit_client.py beside the flood: it creates
one small upload and keeps reading that video, and its p99 and error rate are reported.

--rate bounds requests/s across all worker threads combined (0 means unbounded, limited only by
--workers). Each connection is opened, one request sent, response drained, then closed — no
keep-alive — since the point is connection churn, not throughput per connection.
"""
import argparse
import http.client
import json
import socket
import ssl
import threading
import time
import urllib.parse
import uuid

import legit_client


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


def connect(scheme, host, port):
    kind = http.client.HTTPSConnection if scheme == "https" else http.client.HTTPConnection
    return kind(host, port, timeout=5)


def send_unauth(scheme, host, port):
    conn = connect(scheme, host, port)
    try:
        conn.request("GET", f"/api/v1/videos/{uuid.uuid4()}")
        resp = conn.getresponse()
        resp.read()
        return resp.status
    finally:
        conn.close()


def send_bad(scheme, host, port):
    """A request line the parser should refuse outright: a header name/value pair far past any
    reasonable size, sent raw so nothing in http.client normalizes it away first."""
    sock = socket.create_connection((host, port), timeout=5)
    if scheme == "https":
        # A load tool against a sandbox's self-signed listener, so no verification.
        context = ssl.create_default_context()
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE
        sock = context.wrap_socket(sock, server_hostname=host)
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


def send_valid(scheme, host, port, token):
    conn = connect(scheme, host, port)
    try:
        conn.request("GET", f"/api/v1/videos/{uuid.uuid4()}",
                     headers={"Authorization": f"Bearer {token}"})
        resp = conn.getresponse()
        resp.read()
        return resp.status
    finally:
        conn.close()


def worker(scheme, host, port, token, deadline, rate_gate, flood):
    # Cycle through the shapes so each worker contributes all three kinds rather than one
    # worker per kind, which would skew status counts toward whichever kind has fewer workers
    # when --workers doesn't divide evenly by the number of kinds.
    shapes = [("unauth", lambda: send_unauth(scheme, host, port)), ("bad", lambda: send_bad(scheme, host, port))]
    if token:
        shapes.append(("valid", lambda: send_valid(scheme, host, port, token)))
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
    parser.add_argument("--legit-source", metavar="ADDRESS",
                        help="local address the legit client connects from, e.g. 127.0.0.2, so "
                             "the gateway's per-address limits tell it from the abusive traffic")
    parser.add_argument("--legit-token", help="bearer token for the legit client; required with --legit-url")
    args = parser.parse_args()
    if args.legit_source:
        legit_client.SOURCE_ADDRESS = (args.legit_source, 0)
    if args.legit_url and not args.legit_token:
        parser.error("--legit-url needs --legit-token")

    parts = urllib.parse.urlsplit(args.url)
    flood = Flood()
    deadline = time.monotonic() + args.duration
    rate_gate = RateGate(args.rate) if args.rate > 0 else None

    stop_legit = None
    if args.legit_url:
        stop_legit = legit_client.start(args.legit_url, args.legit_token, args.duration)

    started = time.monotonic()
    threads = [threading.Thread(target=worker,
                                args=(parts.scheme, parts.hostname, parts.port, args.token, deadline,
                                      rate_gate,
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
