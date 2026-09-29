#!/usr/bin/env python3
"""Soak test: gateway_server and transcode_worker under a steady mixed load for hours.

Runs both binaries against the local Postgres and MinIO of deploy/local/compose.yaml, drives a
steady mix of traffic through the gateway, samples each process's resident memory and open
descriptors every minute into samples.csv, and judges at the end whether both stayed flat.

    tests/soak/soak.py --build build/ci --hours 6 --out /some/dir

The binaries are copied into <out>/bin first, so a rebuild of the tree does not change what a
run in progress is measuring. Exit status: 0 flat, 1 not flat, 2 the run itself failed.

The load, per client thread, in proportion (see MIX):
  upload    a small real MP4, created, sent in one chunk and committed; the worker transcodes
            it, and later playlist fetches read it
  resume    a 10 MiB upload whose first PATCH is cut off by closing the socket after 9 MiB,
            resumed from the offset HEAD reports, then cancelled so MinIO frees it
  cancel    a 10 MiB upload with 2 MiB sent, then cancelled
  playlist  master and media playlist of a ready video, and the video's status
  bad       one of: no token, a forged token, a malformed request line, a duplicate Host, an
            unknown route, a wrong method, a wrong offset, bad JSON, an oversized header

Flatness (see judge()): the first WARMUP minutes are excluded; they hold the allocator's and
the connection pools' growth to their working size. Over the rest, a least-squares line is
fitted to each series, and its rise over that window must stay inside two bounds:

  RSS   The slope may not carry the process to its systemd MemoryHigh within 30 days, the
        longest a replica runs between deploys: slope < (MemoryHigh - peak RSS) / 720 h.
        The gateway's MemoryHigh is 600 MB (brief section 3, decision 11); the worker's is
        1800 MB (deploy/systemd/ulw-worker.service). Growth that would take a month to reach
        the limit is indistinguishable, over hours, from the allocator settling; anything
        faster is a leak that pages someone before the next deploy.
  fds   Descriptors do not settle; any sustained rise is a leak. What a sample may
        legitimately differ by is what is in flight: each client holds at most one gateway
        connection, which holds at most one backend socket, so 2 x clients for the gateway;
        the worker runs one job at a time. The fitted rise over the window must stay under
        the larger of that and the range the warm-up already showed. A leak of one descriptor
        per thousand requests at this load's ~5 requests a second is 18 an hour, and fails
        within the first hour of the window.
"""

import argparse
import csv
import hashlib
import http.client
import json
import os
import random
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MIB = 1024 * 1024
WARMUP_MINUTES = 15
MONTH_HOURS = 30 * 24
MEMORY_HIGH = {"gateway": 600 * 1000 * 1000, "worker": 1800 * 1000 * 1000}
# The worker holds one job's descriptors at a time: its two database sessions, the workspace
# files, ffmpeg's pipes, the store's socket.
WORKER_FD_NOISE = 16
# One upload in 25 actions is one every ~12 s: the worker transcodes each in about a second,
# and 6 h of them, raw and HLS, take ~0.5 GB of MinIO's tmpfs.
MIX = [("playlist", 55), ("bad", 25), ("upload", 4), ("resume", 8), ("cancel", 8)]
ISSUER = "ulw-soak"


def log(msg):
    print(time.strftime("%Y-%m-%dT%H:%M:%S"), msg, flush=True)


def env_or(name, default):
    value = os.environ.get(name)
    return value if value else default


class Stack:
    """Postgres database, MinIO bucket, key set and the two processes."""

    def __init__(self, args, out):
        self.args = args
        self.out = out
        self.bin = out / "bin"
        self.name = "ulw_soak_" + time.strftime("%Y%m%d%H%M%S") + "_" + uuid.uuid4().hex[:6]
        self.admin_url = env_or("ULW_TEST_DATABASE_URL",
                                "postgresql://postgres:testtest123@127.0.0.1:55432/postgres")
        self.database_url = self.admin_url.rsplit("/", 1)[0] + "/" + self.name
        self.minio = env_or("ULW_MINIO_ENDPOINT", "http://127.0.0.1:9000")
        self.access = env_or("ULW_MINIO_ACCESS_KEY", "ulw-dev")
        self.secret = env_or("ULW_MINIO_SECRET_KEY", "ulw-dev-secret")
        self.bucket = self.name.replace("_", "-")
        self.port = args.port
        self.procs = {}

    def copy_binaries(self):
        self.bin.mkdir(parents=True, exist_ok=True)
        build = Path(self.args.build).resolve()
        for rel in ["apps/gateway/gateway_server", "apps/worker/transcode_worker",
                    "apps/worker/ulw_sandbox", "apps/migrate/ulw_migrate",
                    "tools/devtoken/ulw_devtoken"]:
            shutil.copy2(build / rel, self.bin / Path(rel).name)

    def s3(self, method, path):
        return subprocess.run(
            ["curl", "-sS", "-o", "/dev/null", "-w", "%{http_code}", "-X", method,
             "--aws-sigv4", "aws:amz:us-east-1:s3", "--user", f"{self.access}:{self.secret}",
             f"{self.minio}/{path}"], capture_output=True, text=True, check=True).stdout

    def prepare(self):
        subprocess.run(["psql", self.admin_url, "-qc", f"CREATE DATABASE {self.name}"],
                       check=True)
        subprocess.run([self.bin / "ulw_migrate"], check=True,
                       env={**os.environ, "ULW_DATABASE_URL": self.database_url},
                       stdout=subprocess.DEVNULL)
        code = self.s3("PUT", self.bucket)
        if code != "200":
            raise RuntimeError(f"creating bucket {self.bucket}: HTTP {code}")
        key = self.out / "dev-key.json"
        subprocess.run([self.bin / "ulw_devtoken", "keygen", key], check=True)
        jwks = subprocess.run([self.bin / "ulw_devtoken", "jwks", key], check=True,
                              capture_output=True, text=True).stdout
        (self.out / "jwks.json").write_text(jwks)
        self.tokens = {}
        for user in ["alice", "bob", "carol", "dave"]:
            self.tokens[user] = subprocess.run(
                [self.bin / "ulw_devtoken", "mint", key, "--iss", ISSUER, "--sub", user,
                 "--ttl", str(7 * 24 * 3600)],
                check=True, capture_output=True, text=True).stdout.strip()

    def common_env(self):
        return {
            "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
            "ULW_DATABASE_URL": self.database_url,
            "ULW_STORAGE": "minio",
            "ULW_S3_ENDPOINT": self.minio,
            "ULW_BUCKET": self.bucket,
            "ULW_S3_ACCESS_KEY_ID": self.access,
            "ULW_S3_SECRET_ACCESS_KEY": self.secret,
        }

    def start(self):
        gateway_env = {**self.common_env(), "ULW_LISTEN_PORT": str(self.port),
                       "ULW_DEV_JWKS_FILE": str(self.out / "jwks.json"), "JWT_ISSUER": ISSUER}
        if os.environ.get("ULW_REACTOR"):
            gateway_env["ULW_REACTOR"] = os.environ["ULW_REACTOR"]
        scratch = self.out / "scratch"
        scratch.mkdir(exist_ok=True)
        worker_env = {**self.common_env(), "ULW_NODE_ID": "soak-worker",
                      "ULW_SCRATCH_DIR": str(scratch), "ULW_FFMPEG_THREADS": "1"}
        for name, program, env in [("gateway", "gateway_server", gateway_env),
                                   ("worker", "transcode_worker", worker_env)]:
            out = open(self.out / f"{name}.log", "ab")
            self.procs[name] = subprocess.Popen([self.bin / program], env=env, stdout=out,
                                                stderr=subprocess.STDOUT)
        deadline = time.time() + 60
        while time.time() < deadline:
            try:
                c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
                c.request("GET", "/readyz")
                if c.getresponse().status == 200:
                    return
            except OSError:
                pass
            time.sleep(0.5)
        raise RuntimeError("gateway never became ready; see gateway.log")

    def pids(self):
        return {name: p.pid for name, p in self.procs.items()}

    def alive(self):
        return all(p.poll() is None for p in self.procs.values())

    def stop(self):
        codes = {}
        for name, p in self.procs.items():
            if p.poll() is None:
                p.send_signal(signal.SIGTERM)
        for name, p in self.procs.items():
            try:
                codes[name] = p.wait(timeout=60)
            except subprocess.TimeoutExpired:
                p.kill()
                codes[name] = "killed"
        return codes

    def cleanup(self):
        subprocess.run(["psql", self.admin_url, "-qc",
                        f"DROP DATABASE IF EXISTS {self.name} WITH (FORCE)"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


class Counts:
    def __init__(self):
        self.lock = threading.Lock()
        self.by_key = {}

    def add(self, key, n=1):
        with self.lock:
            self.by_key[key] = self.by_key.get(key, 0) + n

    def snapshot(self):
        with self.lock:
            return dict(self.by_key)


class Client(threading.Thread):
    """One load client: a keep-alive connection, an action at a time, at a steady pace."""

    def __init__(self, stack, clip, index, counts, stop, ready_videos):
        super().__init__(daemon=True)
        self.stack = stack
        self.clip = clip
        self.rng = random.Random(index)
        self.user = list(stack.tokens)[index % len(stack.tokens)]
        self.token = stack.tokens[self.user]
        self.counts = counts
        self.stop = stop
        self.ready = ready_videos
        self.conn = None

    def http(self, method, path, body=None, headers=None, token=True):
        h = dict(headers or {})
        if token:
            h["Authorization"] = "Bearer " + self.token
        for attempt in range(2):
            if self.conn is None:
                self.conn = http.client.HTTPConnection("127.0.0.1", self.stack.port, timeout=60)
            try:
                self.conn.request(method, path, body=body, headers=h)
                r = self.conn.getresponse()
                data = r.read()
                if r.getheader("Connection", "").lower() == "close":
                    self.conn.close()
                    self.conn = None
                self.counts.add(f"status_{r.status // 100}xx")
                return r.status, r.headers, data
            except (OSError, http.client.HTTPException):
                self.conn.close()
                self.conn = None
                if attempt == 1:
                    self.counts.add("transport_errors")
                    return 0, {}, b""
        return 0, {}, b""

    def create(self, size, name="soak.mp4"):
        body = json.dumps({"filename": name, "size_bytes": size, "content_type": "video/mp4"})
        status, _, data = self.http("POST", "/api/v1/uploads", body,
                                    {"Content-Type": "application/json"})
        if status != 201:
            return None
        return json.loads(data)

    def patch(self, upload, offset, data):
        return self.http("PATCH", f"/api/v1/uploads/{upload}", data,
                         {"Upload-Offset": str(offset)})

    def upload(self):
        up = self.create(len(self.clip))
        if not up:
            return
        status, _, _ = self.patch(up["upload_id"], 0, self.clip)
        if status != 204:
            return
        status, _, _ = self.http("POST", f"/api/v1/uploads/{up['upload_id']}/commit")
        if status == 200:
            self.counts.add("uploads_committed")
            with self.ready.lock:
                self.ready.pending.append((self.user, up["video_id"]))

    def cut_patch(self, upload, total, send):
        """Declares `total` bytes, sends `send` of them, and closes the socket."""
        s = socket.create_connection(("127.0.0.1", self.stack.port), timeout=60)
        try:
            head = (f"PATCH /api/v1/uploads/{upload} HTTP/1.1\r\nHost: soak\r\n"
                    f"Authorization: Bearer {self.token}\r\nUpload-Offset: 0\r\n"
                    f"Content-Length: {total}\r\n\r\n").encode()
            s.sendall(head)
            chunk = os.urandom(MIB)
            for _ in range(send // MIB):
                s.sendall(chunk)
        finally:
            s.close()

    def resume(self):
        total = 10 * MIB
        up = self.create(total, "resume.bin")
        if not up:
            return
        uid = up["upload_id"]
        self.cut_patch(uid, total, 9 * MIB)
        offset = None
        for _ in range(100):
            status, headers, _ = self.http("HEAD", f"/api/v1/uploads/{uid}")
            if status == 204:
                offset = int(headers.get("Upload-Offset", "0"))
                break
            time.sleep(0.1)
        if offset is None:
            return
        conflicts = 0
        while offset < total:
            n = min(total - offset, up["chunk_size"])
            status, headers, _ = self.patch(uid, offset, os.urandom(n))
            # The cut request may still hold the upload for a moment.
            if status == 409 and headers.get("Upload-Offset") and conflicts < 50:
                conflicts += 1
                offset = int(headers["Upload-Offset"])
                time.sleep(0.1)
                continue
            if status != 204:
                return
            offset = int(headers["Upload-Offset"])
        self.counts.add("resumes_completed")
        self.http("DELETE", f"/api/v1/uploads/{uid}")

    def cancel(self):
        up = self.create(10 * MIB, "cancel.bin")
        if not up:
            return
        # Two MiB of a first chunk that will never finish: the store holds nothing durable,
        # and the discard must still release everything the gateway opened for it.
        self.cut_patch(up["upload_id"], 10 * MIB, 2 * MIB)
        status, _, _ = self.http("DELETE", f"/api/v1/uploads/{up['upload_id']}")
        if status == 204:
            self.counts.add("cancels")

    def playlist(self):
        with self.ready.lock:
            mine = [v for u, v in self.ready.done if u == self.user]
        if not mine:
            self.bad()
            return
        video = self.rng.choice(mine)
        self.http("GET", f"/api/v1/videos/{video}")
        status, _, data = self.http("GET", f"/api/v1/videos/{video}/master.m3u8")
        if status != 200:
            return
        # The master names each rendition by the gateway path of its media playlist.
        rungs = [l for l in data.decode().splitlines() if l and not l.startswith("#")]
        if rungs:
            status, _, _ = self.http("GET", self.rng.choice(rungs))
            if status == 200:
                self.counts.add("playlists")

    def raw(self, data):
        s = socket.create_connection(("127.0.0.1", self.stack.port), timeout=30)
        try:
            s.sendall(data)
            s.recv(4096)
        except OSError:
            pass
        finally:
            s.close()

    def bad(self):
        kind = self.rng.randrange(9)
        vid = str(uuid.uuid4())
        if kind == 0:
            self.http("GET", f"/api/v1/videos/{vid}", token=False)
        elif kind == 1:
            self.http("GET", f"/api/v1/videos/{vid}",
                      headers={"Authorization": "Bearer forged.token.value"}, token=False)
        elif kind == 2:
            self.raw(b"GARBAGE / HTTP/9.9\r\n\r\n")
        elif kind == 3:
            self.raw(b"GET /healthz HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n")
        elif kind == 4:
            self.http("GET", "/api/v1/nowhere")
        elif kind == 5:
            self.http("PUT", f"/api/v1/videos/{vid}", b"")
        elif kind == 6:
            up = self.create(MIB, "offset.bin")
            if up:
                self.patch(up["upload_id"], 12345, b"x" * 10)
                self.http("DELETE", f"/api/v1/uploads/{up['upload_id']}")
        elif kind == 7:
            self.http("POST", "/api/v1/uploads", b"{not json",
                      {"Content-Type": "application/json"})
        else:
            self.raw(b"GET /healthz HTTP/1.1\r\nHost: a\r\nX-Big: " + b"a" * 20000 +
                     b"\r\n\r\n")
        self.counts.add("bad_requests")

    def run(self):
        actions = [a for a, w in MIX for _ in range(w)]
        while not self.stop.is_set():
            action = self.rng.choice(actions)
            try:
                getattr(self, action)()
            except Exception as e:  # a client error must not end the run
                self.counts.add("client_exceptions")
                log(f"client: {action}: {e!r}")
                if self.conn:
                    self.conn.close()
                    self.conn = None
            self.counts.add(f"action_{action}")
            # About two actions a second across four clients: steady, not a load test.
            self.stop.wait(self.rng.uniform(1.0, 3.0))


class ReadyVideos:
    """Committed videos, promoted to `done` once the worker has made them ready."""

    def __init__(self):
        self.lock = threading.Lock()
        self.pending = []
        self.done = []

    def poll(self, client):
        with self.lock:
            pending = list(self.pending)
        still = []
        for user, video in pending:
            client.token = client.stack.tokens[user]
            status, _, data = client.http("GET", f"/api/v1/videos/{video}")
            state = json.loads(data).get("state") if status == 200 else None
            if state == "ready":
                with self.lock:
                    self.done.append((user, video))
                    # A bounded working set; older videos stay in the store untouched.
                    self.done = self.done[-200:]
            elif state in ("processing", "uploading", "init"):
                still.append((user, video))
        with self.lock:
            self.pending = [p for p in self.pending if p not in pending] + still


def proc_sample(pid):
    rss = None
    with open(f"/proc/{pid}/status") as f:
        for line in f:
            if line.startswith("VmRSS:"):
                rss = int(line.split()[1])
    fds = len(os.listdir(f"/proc/{pid}/fd"))
    return rss, fds


def fit(points):
    """Least-squares slope (per hour) of (hours, value) points."""
    n = len(points)
    mx = sum(x for x, _ in points) / n
    my = sum(y for _, y in points) / n
    sxx = sum((x - mx) ** 2 for x, _ in points)
    sxy = sum((x - mx) * (y - my) for x, y in points)
    return sxy / sxx if sxx else 0.0


def judge(rows, clients):
    """Returns (passed, report lines) for the samples in `rows`."""
    report = []
    passed = True
    warm = [r for r in rows if r["elapsed_min"] < WARMUP_MINUTES]
    window = [r for r in rows if r["elapsed_min"] >= WARMUP_MINUTES]
    if len(window) < 10:
        return False, ["too few samples after the warm-up to judge"]
    hours = (window[-1]["elapsed_min"] - window[0]["elapsed_min"]) / 60
    report.append(f"window: {len(window)} samples over {hours:.2f} h after a "
                  f"{WARMUP_MINUTES} min warm-up")
    for proc in ["gateway", "worker"]:
        rss = [(r["elapsed_min"] / 60, r[f"{proc}_rss_kb"] * 1024) for r in window]
        fds = [(r["elapsed_min"] / 60, r[f"{proc}_fds"]) for r in window]
        peak = max(v for _, v in rss)
        rss_slope = fit(rss)
        rss_bound = (MEMORY_HIGH[proc] - peak) / MONTH_HOURS
        rss_ok = rss_slope < rss_bound
        fd_slope = fit(fds)
        fd_rise = fd_slope * hours
        warm_fds = [r[f"{proc}_fds"] for r in warm] or [0]
        in_flight = 2 * clients if proc == "gateway" else WORKER_FD_NOISE
        fd_bound = max(in_flight, max(warm_fds) - min(warm_fds))
        fd_ok = fd_rise < fd_bound
        passed = passed and rss_ok and fd_ok
        report.append(
            f"{proc} rss: first {rss[0][1] / MIB:.1f} MiB, last {rss[-1][1] / MIB:.1f} MiB, "
            f"peak {peak / MIB:.1f} MiB, slope {rss_slope / 1e6:+.3f} MB/h, bound "
            f"{rss_bound / 1e6:.3f} MB/h: {'flat' if rss_ok else 'NOT FLAT'}")
        report.append(
            f"{proc} fds: first {fds[0][1]}, last {fds[-1][1]}, min "
            f"{min(v for _, v in fds)}, max {max(v for _, v in fds)}, slope "
            f"{fd_slope:+.3f}/h, rise over window {fd_rise:+.2f}, bound {fd_bound}: "
            f"{'flat' if fd_ok else 'NOT FLAT'}")
    return passed, report


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--build", default=str(ROOT / "build" / "ci"))
    parser.add_argument("--hours", type=float, default=6.0)
    parser.add_argument("--out", required=True)
    parser.add_argument("--port", type=int, default=18180)
    parser.add_argument("--clients", type=int, default=4)
    parser.add_argument("--interval", type=float, default=60.0, help="seconds between samples")
    args = parser.parse_args()

    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    stack = Stack(args, out)
    stack.copy_binaries()
    stack.prepare()
    clip = out / "clip.mp4"
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i",
                    "testsrc2=size=320x240:rate=25", "-f", "lavfi", "-i", "sine=frequency=440",
                    "-t", "2", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac",
                    "-shortest", clip], check=True)
    stack.start()
    pids = stack.pids()
    (out / "pids.json").write_text(json.dumps({**pids, "soak": os.getpid()}))
    log(f"started: gateway pid {pids['gateway']}, worker pid {pids['worker']}, "
        f"database {stack.name}, bucket {stack.bucket}")

    stop = threading.Event()
    counts = Counts()
    ready = ReadyVideos()
    clients = [Client(stack, clip.read_bytes(), i, counts, stop, ready)
               for i in range(args.clients)]
    for c in clients:
        c.start()
    poller = Client(stack, b"", 99, Counts(), stop, ready)

    rows = []
    fields = ["time", "elapsed_min", "gateway_rss_kb", "gateway_fds", "worker_rss_kb",
              "worker_fds", "actions", "status_2xx", "status_4xx", "status_5xx",
              "transport_errors", "uploads_committed", "videos_ready", "resumes_completed",
              "cancels", "playlists"]
    started = time.time()
    end = started + args.hours * 3600
    failure = None
    with open(out / "samples.csv", "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        next_sample = started
        while time.time() < end:
            ready.poll(poller)
            if not stack.alive():
                failure = "a process exited during the run"
                break
            if time.time() >= next_sample:
                g = proc_sample(pids["gateway"])
                w = proc_sample(pids["worker"])
                c = counts.snapshot()
                row = {"time": time.strftime("%Y-%m-%dT%H:%M:%S"),
                       "elapsed_min": round((time.time() - started) / 60, 2),
                       "gateway_rss_kb": g[0], "gateway_fds": g[1],
                       "worker_rss_kb": w[0], "worker_fds": w[1],
                       "actions": sum(v for k, v in c.items() if k.startswith("action_")),
                       "status_2xx": c.get("status_2xx", 0),
                       "status_4xx": c.get("status_4xx", 0),
                       "status_5xx": c.get("status_5xx", 0),
                       "transport_errors": c.get("transport_errors", 0),
                       "uploads_committed": c.get("uploads_committed", 0),
                       "videos_ready": len(ready.done),
                       "resumes_completed": c.get("resumes_completed", 0),
                       "cancels": c.get("cancels", 0),
                       "playlists": c.get("playlists", 0)}
                writer.writerow(row)
                f.flush()
                rows.append(row)
                next_sample += args.interval
            time.sleep(min(5.0, max(0.0, next_sample - time.time())))

    stop.set()
    for c in clients:
        c.join(timeout=120)
    codes = stack.stop()
    stack.cleanup()
    passed, report = judge(rows, args.clients)
    summary = [f"soak: {args.hours} h requested, {len(rows)} samples",
               f"exit codes after SIGTERM: {codes}",
               f"totals: {json.dumps(counts.snapshot(), sort_keys=True)}"] + report
    if failure:
        summary.append(f"FAILED: {failure}")
    verdict = "PASS" if passed and not failure and all(v == 0 for v in codes.values()) else "FAIL"
    summary.append(f"verdict: {verdict}")
    (out / "summary.txt").write_text("\n".join(summary) + "\n")
    print("\n".join(summary), flush=True)
    if failure:
        return 2
    return 0 if verdict == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
