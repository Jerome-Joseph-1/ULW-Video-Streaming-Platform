#!/usr/bin/env python3
"""Drives many concurrent resumable uploads against the gateway (apps/gateway/src/routes.hpp)
and reports throughput, latency and error counts as JSON.

    tests/load/upload_load.py --url http://127.0.0.1:8080 --token-file tokens.txt \
        --uploads 500 --size 16777216

Tokens round-robin over --token-file (one bearer token per line) because the gateway admits at
most 3 uploads per user at once (Limits::max_uploads_per_user, apps/gateway/src/gateway.hpp);
with fewer tokens than concurrent uploads, some connections are expected to see 429 until an
earlier upload of the same user finishes. Rather than a token file, --devtoken-key together with
--issuer and --users mints that many fresh subjects on the fly with the repo's ulw_devtoken
binary (tools/devtoken/src/main.cpp), one token per subject, and round-robins the same way.

Each upload holds one keep-alive connection for its create, PATCHes and commit. It PATCHes the
gateway's chunk_size (from the 201 response) at a time, optionally streamed at --rate bytes/s so
the socket stays open for as long as a slow client's would, then POSTs a commit. Every upload
sends the same bytes: a repeating filler of --size, or --payload, a file.
"""
import argparse
import http.client
import json
import statistics
import subprocess
import sys
import threading
import time
import urllib.parse

# A 4 KiB pattern is far smaller than any chunk but large enough that repeating it never lines
# up with a chunk boundary in a way that would let an over-eager compressor collapse it; nothing
# here compresses, but a non-trivial fill also makes a byte-for-byte truncation obvious in a
# hex dump if a test ever needs one.
FILL = bytes((i * 2654435761) & 0xFF for i in range(4096))


# The gateway answers 408 to a body averaging under 8 KiB/s over a 30 s window
# (Limits::min_body_bytes_per_second and body_rate_window, apps/gateway/src/gateway.hpp). Twice
# that floor leaves room for the pacing granularity and a busy load generator without ever
# tripping it, and holds a 1 MiB upload open for 64 s.
SAFE_RATE = 16 * 1024


def chunk_bytes(n):
    """n bytes built by repeating FILL. Built once per run and shared by every upload."""
    whole, rest = divmod(n, len(FILL))
    return FILL * whole + FILL[:rest]


def mint_tokens(devtoken_bin, key_file, issuer, users):
    tokens = []
    for i in range(users):
        out = subprocess.run(
            [devtoken_bin, "mint", key_file, "--iss", issuer, "--sub", f"load-{i:06d}",
             "--ttl", "3600"],
            check=True, capture_output=True, text=True)
        tokens.append(out.stdout.strip())
    return tokens


class Percentiles:
    """A fixed-size reservoir would drop precision; for the sizes this tool runs at (a few
    thousand PATCHes per run, not millions) keeping every sample and sorting once at the end is
    simpler and cheap enough."""

    def __init__(self):
        self.samples = []
        self.lock = threading.Lock()

    def add(self, value):
        with self.lock:
            self.samples.append(value)

    def summary(self):
        with self.lock:
            data = sorted(self.samples)
        if not data:
            return {"count": 0}
        return {
            "count": len(data),
            "p50_ms": round(statistics.quantiles(data, n=100)[49] * 1000, 2) if len(data) > 1 else round(data[0] * 1000, 2),
            "p95_ms": round(statistics.quantiles(data, n=100)[94] * 1000, 2) if len(data) > 1 else round(data[0] * 1000, 2),
            "p99_ms": round(statistics.quantiles(data, n=100)[98] * 1000, 2) if len(data) > 1 else round(data[0] * 1000, 2),
        }


class Results:
    def __init__(self):
        self.lock = threading.Lock()
        self.attempted = 0
        self.completed = 0
        self.status_counts = {}
        self.errors = {}
        self.bytes_sent = 0
        self.committed = []
        self.patch_latency = Percentiles()

    def record_status(self, status):
        with self.lock:
            self.status_counts[status] = self.status_counts.get(status, 0) + 1

    def record_error(self, kind):
        with self.lock:
            self.errors[kind] = self.errors.get(kind, 0) + 1

    def add_bytes(self, n):
        with self.lock:
            self.bytes_sent += n

    def to_dict(self):
        with self.lock:
            return {
                "attempted": self.attempted,
                "completed": self.completed,
                "status_counts": dict(self.status_counts),
                "errors_by_kind": dict(self.errors),
                "total_bytes_sent": self.bytes_sent,
                "committed_videos": list(self.committed),
            }


def connect(base, timeout):
    kind = http.client.HTTPSConnection if base.scheme == "https" else http.client.HTTPConnection
    return kind(base.hostname, base.port, timeout=timeout)


def request(base, method, path, token=None, body=None, headers=None, timeout=60, conn=None):
    """One request. With conn it goes over that keep-alive connection and leaves it open;
    without, over a connection of its own."""
    own = conn is None
    if own:
        conn = connect(base, timeout)
    try:
        all_headers = dict(headers or {})
        if token:
            all_headers["Authorization"] = f"Bearer {token}"
        if isinstance(body, dict):
            body = json.dumps(body).encode()
            all_headers["Content-Type"] = "application/json"
        conn.request(method, path, body=body, headers=all_headers)
        response = conn.getresponse()
        data = response.read()
        return response.status, {k.lower(): v for k, v in response.getheaders()}, data
    finally:
        if own:
            conn.close()


def patch_streamed(conn, path, token, offset, piece, rate):
    """A PATCH whose body leaves at `rate` bytes/s over the open connection, so the gateway holds
    the upload's socket and buffers for as long as the chunk takes, as it does for a slow client.
    Sent in quarter-second pieces against a deadline, so a stall is caught up rather than added
    to."""
    conn.putrequest("PATCH", path)
    conn.putheader("Authorization", f"Bearer {token}")
    conn.putheader("Upload-Offset", str(offset))
    conn.putheader("Content-Type", "application/offset+octet-stream")
    conn.putheader("Content-Length", str(len(piece)))
    conn.endheaders()
    step = max(1, rate // 4)
    started = time.monotonic()
    try:
        for sent in range(0, len(piece), step):
            conn.send(piece[sent:sent + step])
            ahead = min(sent + step, len(piece)) / rate - (time.monotonic() - started)
            if ahead > 0:
                time.sleep(ahead)
    except OSError:
        # The gateway answers a body it refuses (408, 503) and closes; the answer is what the
        # run should record, not the broken pipe that follows it. When none was sent,
        # getresponse raises the error that is.
        pass
    response = conn.getresponse()
    data = response.read()
    return response.status, {k.lower(): v for k, v in response.getheaders()}, data


def run_one_upload(base, token, size, chunk_cap, rate, results, payload, index):
    with results.lock:
        results.attempted += 1
    conn = connect(base, 120)
    try:
        upload(conn, base, token, size, chunk_cap, rate, results, payload, index)
    finally:
        conn.close()


def upload(conn, base, token, size, chunk_cap, rate, results, payload, index):
    try:
        status, _, data = request(base, "POST", "/api/v1/uploads", token,
                                  {"filename": "load.mp4", "size_bytes": size,
                                   "content_type": "video/mp4"}, conn=conn)
    except (OSError, http.client.HTTPException) as e:
        results.record_error(f"create:{type(e).__name__}")
        return
    if status == 429:
        results.record_status(429)
        return
    if status == 503:
        results.record_status(503)
        return
    if status != 201:
        results.record_status(status)
        results.record_error(f"create:http_{status}")
        return
    results.record_status(status)
    created = json.loads(data)
    upload_id = created["upload_id"]
    chunk_size = min(created["chunk_size"], chunk_cap) if chunk_cap else created["chunk_size"]
    offset = created["durable_offset"]
    body = payload
    while offset < size:
        piece = body[offset:offset + chunk_size]
        piece_started = time.monotonic()
        try:
            if rate:
                status, headers, data = patch_streamed(
                    conn, f"/api/v1/uploads/{upload_id}", token, offset, piece, rate)
            else:
                status, headers, data = request(
                    base, "PATCH", f"/api/v1/uploads/{upload_id}", token, piece,
                    {"Upload-Offset": str(offset),
                     "Content-Type": "application/offset+octet-stream"}, conn=conn)
        except (OSError, http.client.HTTPException) as e:
            results.record_error(f"patch:{type(e).__name__}")
            return
        results.patch_latency.add(time.monotonic() - piece_started)
        results.record_status(status)
        if status == 429 or status == 503:
            return
        if status != 204:
            results.record_error(f"patch:http_{status}")
            return
        offset = int(headers["upload-offset"])
        results.add_bytes(len(piece))
    try:
        status, _, data = request(base, "POST", f"/api/v1/uploads/{upload_id}/commit", token,
                                  conn=conn)
    except (OSError, http.client.HTTPException) as e:
        results.record_error(f"commit:{type(e).__name__}")
        return
    results.record_status(status)
    if status != 200:
        results.record_error(f"commit:http_{status}")
        return
    with results.lock:
        results.completed += 1
        results.committed.append({"video_id": created["video_id"], "token_index": index})


def scrape_metrics_loop(base, stop_event, series):
    """Samples /metrics every 5 s (the interval the task asked for) until stop_event fires, and
    records the two gauges apps/gateway/src/gateway.cpp's render_metrics exposes that describe
    load in flight: uploads_in_flight and connections_current. The gateway build in this repo
    exposes no process_resident_memory_bytes metric, so it is omitted rather than invented."""
    parts = urllib.parse.urlsplit(base)
    while not stop_event.is_set():
        try:
            status, _, data = request(parts, "GET", "/metrics", timeout=10)
            if status == 200:
                text = data.decode()
                sample = {"t": time.time()}
                for name in ("uploads_in_flight", "connections_current"):
                    for line in text.splitlines():
                        if line.startswith(name + " "):
                            sample[name] = int(line.split()[-1])
                series.append(sample)
        except OSError:
            pass
        stop_event.wait(5)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--url", required=True, help="gateway base URL, e.g. http://127.0.0.1:8080")
    parser.add_argument("--uploads", type=int, default=500)
    parser.add_argument("--size", type=int, default=16 * (1 << 20))
    parser.add_argument("--rate", type=int, default=0,
                        help=f"bytes/s per connection, streamed over the open socket; 0 means as fast as "
                             f"possible; {SAFE_RATE} is the slowest rate that keeps clear of the "
                             "gateway's minimum")
    parser.add_argument("--payload", help="upload this file's bytes (and its size) instead of "
                                          "generated filler")
    parser.add_argument("--token-file", help="one bearer token per line, round-robined")
    parser.add_argument("--devtoken-key", help="ulw_devtoken private key file")
    parser.add_argument("--devtoken", default="build/ci/tools/devtoken/ulw_devtoken",
                        help="path to the ulw_devtoken binary")
    parser.add_argument("--issuer", help="JWT_ISSUER to mint with --devtoken-key")
    parser.add_argument("--users", type=int, default=0,
                        help="distinct subjects to mint with --devtoken-key")
    parser.add_argument("--concurrency", type=int, default=None,
                        help="threads running uploads at once; defaults to --uploads")
    parser.add_argument("--scrape-metrics", help="gateway base URL to sample /metrics from")
    args = parser.parse_args()

    if bool(args.token_file) == bool(args.devtoken_key):
        sys.exit("upload_load: give exactly one of --token-file or --devtoken-key")

    if args.token_file:
        with open(args.token_file) as f:
            tokens = [line.strip() for line in f if line.strip()]
        if not tokens:
            sys.exit("upload_load: --token-file has no tokens")
    else:
        if not args.issuer or not args.users:
            sys.exit("upload_load: --devtoken-key needs --issuer and --users")
        tokens = mint_tokens(args.devtoken, args.devtoken_key, args.issuer, args.users)

    if args.payload:
        with open(args.payload, "rb") as f:
            payload = f.read()
        args.size = len(payload)
    else:
        payload = chunk_bytes(args.size)
    # One buffer for every upload, sliced without copying: 500 uploads of 16 MiB would otherwise
    # hold 8 GiB of client memory.
    payload = memoryview(payload)

    base = urllib.parse.urlsplit(args.url)
    results = Results()
    metrics_series = []
    stop_scrape = threading.Event()
    scrape_thread = None
    if args.scrape_metrics:
        scrape_thread = threading.Thread(
            target=scrape_metrics_loop, args=(args.scrape_metrics, stop_scrape, metrics_series),
            daemon=True)
        scrape_thread.start()

    concurrency = args.concurrency or args.uploads
    wall_started = time.monotonic()
    threads = []
    # A simple semaphore-bounded pool: one thread per upload, capped at --concurrency in flight,
    # which is enough for the sizes this tool targets (hundreds, not thousands) and keeps the
    # implementation to plain threads rather than a selector-driven event loop.
    sem = threading.Semaphore(concurrency)

    def worker(i):
        with sem:
            token = tokens[i % len(tokens)]
            run_one_upload(base, token, args.size, None, args.rate, results, payload,
                           i % len(tokens))

    for i in range(args.uploads):
        t = threading.Thread(target=worker, args=(i,))
        t.start()
        threads.append(t)
    for t in threads:
        t.join()

    wall_time = time.monotonic() - wall_started
    stop_scrape.set()
    if scrape_thread:
        scrape_thread.join(timeout=10)

    report = results.to_dict()
    report["wall_time_s"] = round(wall_time, 3)
    report["patch_latency"] = results.patch_latency.summary()
    report["rejections_429"] = report["status_counts"].get(429, 0)
    report["rejections_503"] = report["status_counts"].get(503, 0)
    if metrics_series:
        report["metrics_samples"] = metrics_series
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
