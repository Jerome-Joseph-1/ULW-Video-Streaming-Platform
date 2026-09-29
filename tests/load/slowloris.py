#!/usr/bin/env python3
"""Opens many sockets to the gateway and trickles bytes into them against the timeouts the
gateway documents for a connection (apps/gateway/src/gateway.hpp's Limits, enforced in
apps/gateway/src/connection.cpp):

  header_timeout     10 s, measured from the request's first byte, not its latest: a client that
                      drips the header block one byte at a time is cut off at 10 s all the same
                      (Connection's timer, "Measured from the request's first byte")
  body_idle_timeout   30 s of no forward progress on a request body
  min_body_bytes_per_second / body_rate_window
                      a body must average at least 8 KiB/s (8 * 1024 B/s) over each 30 s window
                      it is read in, or the gateway answers 408 (Connection::check_body_rate)

Two shapes, chosen with --mode:
  headers   opens a connection, sends the header block one byte every --byte-interval seconds
            and never finishes it. The gateway should close each connection about 10 s in, so
            the report's closed_after_buckets show whether the timeout is enforced, and how many
            sockets it held meanwhile.
  body      needs --token-file. Creates a real upload per connection (a 64 KiB one, with a token
            round-robined from the file: the gateway admits 3 uploads per user), sends the PATCH
            header block, then trickles the body at --body-rate bytes/s. Below 8 KiB/s the
            gateway answers 408 once a 30 s window has passed, which is the floor being reached;
            at or above it the connection lives.

    tests/load/slowloris.py --url http://127.0.0.1:8080 --connections 50 --duration 60 \
        --legit-url http://127.0.0.1:8080 --legit-token "$TOKEN"

Reports, as JSON: connections opened, how many the gateway closed (and after how long, in
buckets), and - with --legit-url - the concurrent legitimate client's latency and error rate
(legit_client.py), to show its p99 holds up while the slow connections are open.
"""
import argparse
import http.client
import json
import socket
import ssl
import threading
import time
import urllib.parse

import legit_client

# A byte every 4 s keeps a connection alive on the idle path only if the timeout were per byte;
# the gateway measures header_timeout (10 s) from the first byte, so these are closed at 10 s.
DEFAULT_BYTE_INTERVAL_S = 4.0
# Comfortably under the 8 KiB/s = 8192 B/s floor (min_body_bytes_per_second): a tenth of it
# guarantees the body-rate check trips well within its 30 s window rather than racing it.
DEFAULT_BODY_RATE_BPS = 800


# Set from an https --url: every slow connection then completes a TLS handshake first, so what
# it trickles is HTTP, not a stalled handshake. A load tool against a sandbox's self-signed
# listener, so nothing is verified.
TLS_CONTEXT = None


def dial(host, port):
    sock = socket.create_connection((host, port), timeout=10)
    if TLS_CONTEXT is None:
        return sock
    try:
        return TLS_CONTEXT.wrap_socket(sock, server_hostname=host)
    except OSError:
        sock.close()
        raise


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
        sock = dial(host, port)
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


def create_upload(host, port, token, size):
    if TLS_CONTEXT is None:
        conn = http.client.HTTPConnection(host, port, timeout=10)
    else:
        conn = http.client.HTTPSConnection(host, port, timeout=10, context=TLS_CONTEXT)
    try:
        conn.request("POST", "/api/v1/uploads",
                     body=json.dumps({"filename": "slow.mp4", "size_bytes": size,
                                      "content_type": "video/mp4"}),
                     headers={"Authorization": f"Bearer {token}",
                              "Content-Type": "application/json"})
        response = conn.getresponse()
        data = response.read()
    finally:
        conn.close()
    return json.loads(data)["upload_id"] if response.status == 201 else None


def slow_body_connection(host, port, body_rate, token, deadline, stats):
    body_len = 64 * 1024
    try:
        upload_id = create_upload(host, port, token, body_len)
    except OSError:
        upload_id = None
    if upload_id is None:
        with stats.lock:
            stats.connect_errors += 1
        return
    started = time.monotonic()
    try:
        sock = dial(host, port)
    except OSError:
        with stats.lock:
            stats.connect_errors += 1
        return
    with stats.lock:
        stats.opened += 1
    try:
        header = (
            f"PATCH /api/v1/uploads/{upload_id} HTTP/1.1\r\n"
            "Host: load-test\r\n"
            f"Authorization: Bearer {token}\r\n"
            "Upload-Offset: 0\r\n"
            "Content-Type: application/offset+octet-stream\r\n"
            f"Content-Length: {body_len}\r\n\r\n"
        ).encode()
        sock.sendall(header)
        sent = 0
        while sent < body_len and time.monotonic() < deadline:
            sock.sendall(b"x")
            sent += 1
            # Deliberate pacing to the target rate - the point of this tool, not a
            # wait-for-condition sleep.
            behind = sent / body_rate - (time.monotonic() - started)
            if behind > 0:
                time.sleep(behind)
        # A 408 or a closed socket ends the measurement; so does the deadline.
        sock.settimeout(max(1.0, deadline - time.monotonic()))
        try:
            data = sock.recv(4096)
        except OSError:
            data = b""
        if data == b"" or data.startswith(b"HTTP/1.1 408"):
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
    parser.add_argument("--token-file", help="body mode: one bearer token per line, "
                                              "round-robined (3 uploads per user at most)")
    parser.add_argument("--legit-url", help="base URL for a concurrent legit client loop")
    parser.add_argument("--legit-source", metavar="ADDRESS",
                        help="local address the legit client connects from, e.g. 127.0.0.2, so "
                             "the gateway's per-address limits tell it from the abusive traffic")
    parser.add_argument("--legit-token", help="bearer token for the legit client; required "
                                              "with --legit-url")
    args = parser.parse_args()
    if args.legit_source:
        legit_client.SOURCE_ADDRESS = (args.legit_source, 0)
    if args.legit_url and not args.legit_token:
        parser.error("--legit-url needs --legit-token")
    tokens = []
    if args.mode == "body":
        if not args.token_file:
            parser.error("--mode body needs --token-file")
        with open(args.token_file) as f:
            tokens = [line.strip() for line in f if line.strip()]
        if len(tokens) * 3 < args.connections:
            parser.error(f"{args.connections} connections need at least "
                         f"{-(-args.connections // 3)} tokens: the gateway admits 3 uploads a user")

    parts = urllib.parse.urlsplit(args.url)
    if parts.scheme == "https":
        global TLS_CONTEXT
        TLS_CONTEXT = ssl.create_default_context()
        TLS_CONTEXT.check_hostname = False
        TLS_CONTEXT.verify_mode = ssl.CERT_NONE
    stats = SlowlorisStats()
    deadline = time.monotonic() + args.duration

    stop_legit = None
    if args.legit_url:
        stop_legit = legit_client.start(args.legit_url, args.legit_token, args.duration)

    started = time.monotonic()
    threads = []
    for i in range(args.connections):
        if args.mode == "headers":
            target, extra = slow_header_connection, (args.byte_interval, deadline, stats)
        else:
            target = slow_body_connection
            extra = (args.body_rate, tokens[i % len(tokens)], deadline, stats)
        t = threading.Thread(target=target, args=(parts.hostname, parts.port, *extra))
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
