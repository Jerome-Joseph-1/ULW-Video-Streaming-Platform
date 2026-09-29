#!/usr/bin/env python3
"""Opens many sockets to the gateway and trickles bytes into them just under the timeouts the
gateway documents for a connection (apps/gateway/src/gateway.hpp's Limits, enforced in
apps/gateway/src/connection.cpp):

  header_timeout     10 s from the first byte of a request to a complete header block
                      (arm_timer(header_timeout) in Connection::on_head / the header-wait path)
  body_idle_timeout   30 s of no forward progress on a request body
  min_body_bytes_per_second / body_rate_window
                      a body must average at least 8 KiB/s (8 * 1024 B/s) over each 30 s window
                      it is read in, or the gateway answers 408 (Connection::check_body_rate)

Two attack shapes, chosen with --mode:
  headers   opens a connection, sends one header line, then a further byte every
            --byte-interval seconds (comfortably under 10 s) forever — this never crosses the
            per-byte header timeout but never finishes the header block either, so it measures
            how many connections the gateway will hold open on unfinished headers, not whether
            the timeout itself is enforced correctly.
  body      completes a small PATCH's header block, then trickles the body at a rate the caller
            sets with --body-rate (bytes/s); set it below 8 KiB/s to also exercise the 30 s
            floor rather than only the idle timeout.

    tests/load/slowloris.py --url http://127.0.0.1:8080 --connections 50 --duration 60 \
        --legit-url http://127.0.0.1:8080

Reports, as JSON: connections opened, how many the gateway closed (and after how long, in
buckets), and — with --legit-url — the concurrent legitimate client's latency and error rate, to
show its p99 holds up while the slow connections are open.
"""
import argparse
import http.client
import json
import socket
import statistics
import threading
import time
import urllib.parse

# Comfortably under the 10 s header_timeout (apps/gateway/src/gateway.hpp): a byte every 4 s
# means at most 3 bytes could still be in flight when the timer would fire, so this mode's
# connections are never at risk of being closed as a header-timeout false positive.
DEFAULT_BYTE_INTERVAL_S = 4.0
# Comfortably under the 8 KiB/s = 8192 B/s floor (min_body_bytes_per_second): a tenth of it
# guarantees the body-rate check trips well within its 30 s window rather than racing it.
DEFAULT_BODY_RATE_BPS = 800


class SlowlorisStats:
    def __init__(self):
        self.lock = threading.Lock()
        self.opened = 0
        self.closed_at_s = []  # seconds each closed connection survived
        self.still_open = 0
        self.connect_errors = 0

    def record_close(self, survived_s):
        with self.lock:
            self.closed_at_s.append(survived_s)

    def to_dict(self, duration):
        with self.lock:
            closed = list(self.closed_at_s)
        buckets = {"<10s": 0, "10-30s": 0, "30-60s": 0, ">60s": 0}
        for s in closed:
            if s < 10:
                buckets["<10s"] += 1
            elif s < 30:
                buckets["10-30s"] += 1
            elif s < 60:
                buckets["30-60s"] += 1
            else:
                buckets[">60s"] += 1
        return {
            "opened": self.opened,
            "connect_errors": self.connect_errors,
            "closed_by_gateway": len(closed),
            "closed_after_buckets": buckets,
            "still_open_at_end": self.still_open,
            "run_duration_s": round(duration, 3),
        }


def slow_header_connection(host, port, byte_interval, deadline, stats):
    # A single line at a time, not a full request: this is meant to sit in the header-parsing
    # state rather than complete one, the shape slowloris-style probes use.
    lines = [
        b"GET /api/v1/healthz HTTP/1.1\r\n",
        b"Host: load-test\r\n",
        b"X-Slowloris: 1\r\n",
        # Never sent: an empty line would complete the header block, and closing it would be a
        # real request, not the stall this mode measures.
    ]
    started = time.monotonic()
    try:
        sock = socket.create_connection((host, port), timeout=10)
    except OSError:
        with stats.lock:
            stats.connect_errors += 1
        return
    with stats.lock:
        stats.opened += 1
    try:
        sock.settimeout(byte_interval + 5)
        for line in lines:
            for i in range(len(line)):
                if time.monotonic() >= deadline:
                    with stats.lock:
                        stats.still_open += 1
                    return
                sock.sendall(line[i:i + 1])
                time.sleep(byte_interval)
        # Header lines exhausted: idle without ever sending the closing CRLF, still under
        # observation until the run's deadline or the gateway's own timeout, whichever is first.
        while time.monotonic() < deadline:
            sock.settimeout(min(byte_interval, deadline - time.monotonic()))
            try:
                chunk = sock.recv(1)
            except socket.timeout:
                continue
            if chunk == b"":
                stats.record_close(time.monotonic() - started)
                return
        with stats.lock:
            stats.still_open += 1
    except OSError:
        stats.record_close(time.monotonic() - started)
    finally:
        try:
            sock.close()
        except OSError:
            pass


def slow_body_connection(host, port, body_rate, deadline, stats):
    body_len = 64 * 1024  # one small PATCH body, never actually completed at this rate
    started = time.monotonic()
    try:
        sock = socket.create_connection((host, port), timeout=10)
    except OSError:
        with stats.lock:
            stats.connect_errors += 1
        return
    with stats.lock:
        stats.opened += 1
    try:
        header = (
            "PATCH /api/v1/uploads/00000000-0000-7000-8000-000000000000 HTTP/1.1\r\n"
            "Host: load-test\r\n"
            "Upload-Offset: 0\r\n"
            "Content-Type: application/offset+octet-stream\r\n"
            f"Content-Length: {body_len}\r\n\r\n"
        ).encode()
        sock.sendall(header)
        sent = 0
        piece = b"x"
        while sent < body_len and time.monotonic() < deadline:
            sock.sendall(piece)
            sent += 1
            # Deliberate pacing to the target rate — the point of this tool, not a
            # wait-for-condition sleep.
            behind = sent / body_rate - (time.monotonic() - started)
            if behind > 0:
                time.sleep(behind)
        # Whatever the server answers (401 for the bogus upload id, or a timeout closing the
        # socket first) ends this connection's measurement the same way.
        sock.settimeout(max(1.0, deadline - time.monotonic()))
        try:
            data = sock.recv(4096)
        except OSError:
            data = b""
        if data == b"":
            stats.record_close(time.monotonic() - started)
        else:
            with stats.lock:
                stats.still_open += 1
    except OSError:
        stats.record_close(time.monotonic() - started)
    finally:
        try:
            sock.close()
        except OSError:
            pass


def run_legit_client(url, token, duration):
    stop_event = threading.Event()
    latencies = []
    errors = []
    lock = threading.Lock()

    def guarded_loop():
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

    thread = threading.Thread(target=guarded_loop, daemon=True)
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
    parser.add_argument("--connections", type=int, default=200)
    parser.add_argument("--duration", type=float, default=60.0)
    parser.add_argument("--mode", choices=["headers", "body"], default="headers")
    parser.add_argument("--byte-interval", type=float, default=DEFAULT_BYTE_INTERVAL_S,
                        help="headers mode: seconds between bytes of the header block")
    parser.add_argument("--body-rate", type=float, default=DEFAULT_BODY_RATE_BPS,
                        help="body mode: bytes/s to trickle the PATCH body at")
    parser.add_argument("--legit-url", help="base URL for a concurrent legit client loop")
    parser.add_argument("--legit-token", help="bearer token for the legit client, if it needs one")
    args = parser.parse_args()

    parts = urllib.parse.urlsplit(args.url)
    stats = SlowlorisStats()
    deadline = time.monotonic() + args.duration

    stop_legit = None
    if args.legit_url:
        stop_legit = run_legit_client(args.legit_url, args.legit_token, args.duration)

    target = slow_header_connection if args.mode == "headers" else slow_body_connection
    extra = (args.byte_interval,) if args.mode == "headers" else (args.body_rate,)

    started = time.monotonic()
    threads = []
    for _ in range(args.connections):
        t = threading.Thread(target=target, args=(parts.hostname, parts.port, *extra, deadline, stats))
        t.start()
        threads.append(t)
    for t in threads:
        t.join()
    wall = time.monotonic() - started

    report = stats.to_dict(wall)
    if stop_legit:
        report["legit_client"] = stop_legit()
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
