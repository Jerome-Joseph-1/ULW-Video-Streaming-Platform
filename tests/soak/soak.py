#!/usr/bin/env python3
"""Soak test: gateway_server and transcode_worker under a steady mixed load for hours.

Runs both binaries against the local Postgres and MinIO of deploy/local/compose.yaml, the
gateway serving TLS, drives a steady mix of traffic through it, samples each process's resident
memory and open descriptors every minute into samples.csv, and judges at the end whether both
stayed flat.

    tests/soak/soak.py --build build/ci --hours 6 --out /some/dir
    tests/soak/soak.py --rejudge /some/dir/samples.csv      # a finished run, again

The binaries are copied into <out>/bin first, so a rebuild of the tree does not change what a
run in progress is measuring. Exit status: 0 flat, 1 not flat, 2 the run itself failed.

The load:
  clients     --clients threads, each a keep-alive TLS connection, choosing from MIX a few
              times a second: a ready video's master and media playlists and status, the offset
              of an upload, and bad requests (no token, a forged one, a malformed request line,
              a duplicate Host, an unknown route, a wrong method, a wrong offset, bad JSON, an
              oversized header)
  uploads     one thread, one upload session every 3 s: a small real MP4 created, sent and
              committed (the worker transcodes it, and playlist fetches then read it), or a
              10 MiB upload cut off after 9 MiB and resumed from the offset HEAD reports, or
              one cancelled after 2 MiB; resumed and cancelled ones are discarded, so MinIO
              frees them
  slow        a slow client every 20 s: a head that never ends (header timeout), a body that
              stops (body idle timeout), a body dripped below the minimum rate (body rate)
  saturation  every 60 s four users hold three chunk uploads each, filling the gateway's 12
              upload slots (ULW_MAX_UPLOAD_SLOTS), after the first has asked for a fourth (429:
              its own limit), and then another asks for one more (503: the gateway's)
  store       every 120 s one ready video's media playlist is deleted from the store behind the
              gateway's back, then fetched
  SIGHUP      every 10 minutes the gateway reloads its certificate

Not driven: the JWKS fetch path (the gateway verifies against a local key set) and database
outages (the database is shared with other work).

Flatness, judge(): the first WARMUP_MINUTES are excluded; they hold the allocator's and the
connection pools' growth to their working size. Over the rest a least-squares line is fitted to
each series, and the upper end of its 95% confidence interval is what is judged, so a run too
short or too quiet to tell fails rather than passes.

  RSS   Normalised by the work that could leak: the gateway per request and per upload
        session, the worker per job. A process is flat when that upper bound would not carry it
        from its peak to its systemd MemoryHigh within 30 days at production rates:
            leak per unit < (MemoryHigh - peak) / (production rate x 720 h)
        MemoryHigh: 600 MB gateway (brief decision 11), 1800 MB worker (its unit). Production
        rates, at the ceiling rather than on average:
          requests        670/s: 448 upload slots (ADR-0027) each sending an 8 MiB chunk every
                          0.67 s, the time it takes at 100 Mbit/s; playlists and control calls
                          are few beside them
          upload sessions 5.3/s: a 100 MiB upload over 10 Mbit/s holds its slot 84 s, and 448
                          slots / 84 s is 5.3 sessions a second
          jobs            1.67/s: the soak's two-second clip takes the worker 0.6 s end to
                          end, the least any job takes, and 1 / 0.6 s is 1.67 jobs a second
        With a gateway peak of ~40 MB that is 0.32 bytes per request and 41 bytes per upload
        session: one leaked allocation (32 bytes at least) on every hundredth request fails. A
        worker peak of ~16 MB gives 413 bytes per job.
        Chunk requests are also judged on their own, against the same 670/s: they are what that
        ceiling counts, and in the soak they are a small share of all requests, so a leak on
        each chunk would be diluted per request by the playlists and control calls around them.
  fds   Descriptors do not settle; any sustained rise is a leak. What a sample may legitimately
        differ by is what is in flight: each client and the uploads thread hold a connection,
        and each connection at most one backend socket; the slow clients (SLOW_MAX at once)
        and the saturation holds (UPLOAD_SLOTS + 1) theirs. The worker runs one job at a time
        and holds at most WORKER_FD_NOISE for it. The upper end of the fitted rise over the
        window must stay under that bound, or the range the warm-up already showed if larger.

--rejudge also gives the verdict of the criterion this one replaced (slope per hour against
MemoryHigh over 30 days, not normalised), so earlier runs can be read both ways.
"""

import argparse
import csv
import http.client
import json
import math
import os
import random
import shutil
import signal
import socket
import ssl
import subprocess
import sys
import threading
import time
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MIB = 1024 * 1024
WARMUP_MINUTES = 15
MONTH_SECONDS = 30 * 24 * 3600
MEMORY_HIGH = {"gateway": 600 * 1000 * 1000, "worker": 1800 * 1000 * 1000}
PRODUCTION_REQUESTS_PER_S = 670.0
PRODUCTION_UPLOADS_PER_S = 5.3
PRODUCTION_JOBS_PER_S = 1 / 0.6
# Two-sided 95%: the slope's upper end is the slope plus 1.96 standard errors.
Z95 = 1.96
# The worker holds one job's descriptors at a time: its two database sessions, the workspace
# files, ffmpeg's pipes, the store's socket.
WORKER_FD_NOISE = 16
# Slow clients last up to ~80 s and one starts every 20 s.
SLOW_MAX = 4
UPLOAD_SLOTS = 12
MIX = [("playlist", 45), ("status", 15), ("offset", 10), ("bad", 30)]
ISSUER = "ulw-soak"
USERS = ["alice", "bob", "carol", "dave"]
HOLDERS = ["sat1", "sat2", "sat3", "sat4"]
# Its own user, so a slow body never takes one of the saturation holders' slots.
SLOW_USER = "slowpoke"


def log(msg):
    print(time.strftime("%Y-%m-%dT%H:%M:%S"), msg, flush=True)


def env_or(name, default):
    value = os.environ.get(name)
    return value if value else default


class Stack:
    """Postgres database, MinIO bucket, keys, certificate and the two processes."""

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
        self.tls = None
        self.tokens = {}

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
                       env={**os.environ, "ULW_DATABASE_URL": self.database_url,
                            "ULW_ALLOW_ROOT": "1"},
                       stdout=subprocess.DEVNULL)
        code = self.s3("PUT", self.bucket)
        if code != "200":
            raise RuntimeError(f"creating bucket {self.bucket}: HTTP {code}")
        key = self.out / "dev-key.json"
        subprocess.run([self.bin / "ulw_devtoken", "keygen", key], check=True)
        jwks = subprocess.run([self.bin / "ulw_devtoken", "jwks", key], check=True,
                              capture_output=True, text=True).stdout
        (self.out / "jwks.json").write_text(jwks)
        for user in USERS + HOLDERS + [SLOW_USER]:
            self.tokens[user] = subprocess.run(
                [self.bin / "ulw_devtoken", "mint", key, "--iss", ISSUER, "--sub", user,
                 "--ttl", str(7 * 24 * 3600)],
                check=True, capture_output=True, text=True).stdout.strip()
        subprocess.run(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt",
                        "ec_paramgen_curve:P-256", "-nodes", "-days", "30", "-subj",
                        "/CN=127.0.0.1", "-addext", "subjectAltName=IP:127.0.0.1",
                        "-keyout", self.out / "key.pem", "-out", self.out / "cert.pem"],
                       check=True, capture_output=True)
        self.tls = ssl.create_default_context(cafile=str(self.out / "cert.pem"))

    def common_env(self):
        return {
            "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
            "ULW_DATABASE_URL": self.database_url,
            "ULW_STORAGE": "minio",
            "ULW_S3_ENDPOINT": self.minio,
            "ULW_BUCKET": self.bucket,
            "ULW_S3_ACCESS_KEY_ID": self.access,
            "ULW_S3_SECRET_ACCESS_KEY": self.secret,
            # The soak may run as root on a development host.
            "ULW_ALLOW_ROOT": "1",
        }

    def connection(self, timeout=60):
        return http.client.HTTPSConnection("127.0.0.1", self.port, timeout=timeout,
                                           context=self.tls)

    def raw_socket(self, timeout=120):
        s = socket.create_connection(("127.0.0.1", self.port), timeout=timeout)
        return self.tls.wrap_socket(s, server_hostname="127.0.0.1")

    def start(self):
        gateway_env = {**self.common_env(), "ULW_LISTEN_PORT": str(self.port),
                       "ULW_DEV_JWKS_FILE": str(self.out / "jwks.json"), "ULW_DEV_MODE": "1",
                       "JWT_ISSUER": ISSUER,
                       "ULW_TRANSPORT": "tls", "ULW_TLS_CERT_FILE": str(self.out / "cert.pem"),
                       "ULW_TLS_KEY_FILE": str(self.out / "key.pem"),
                       "ULW_MAX_UPLOAD_SLOTS": str(UPLOAD_SLOTS),
                       # One address and four users stand in for every client, and the soak
                       # fills every slot at once: the per-client limits, which have tests of
                       # their own, would refuse its load rather than serve it.
                       "ULW_MAX_CONNECTIONS_PER_IP": "448",
                       "ULW_NEW_CONNECTIONS_PER_IP_PER_SECOND": "65536",
                       "ULW_REQUESTS_PER_USER_PER_MINUTE": "1000000"}
        if os.environ.get("ULW_REACTOR"):
            gateway_env["ULW_REACTOR"] = os.environ["ULW_REACTOR"]
        # Experiments only: an allocator to put in front of glibc's.
        if os.environ.get("ULW_SOAK_PRELOAD"):
            gateway_env["LD_PRELOAD"] = os.environ["ULW_SOAK_PRELOAD"]
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
                c = self.connection(timeout=5)
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
        for p in self.procs.values():
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


class ReadyVideos:
    """Committed videos, promoted to `done` once the worker has made them ready."""

    def __init__(self):
        self.lock = threading.Lock()
        self.pending = []
        self.done = []

    def add(self, user, video):
        with self.lock:
            self.pending.append((user, video))

    def pick(self, rng, user=None):
        with self.lock:
            mine = [v for u, v in self.done if user is None or u == user]
        return rng.choice(mine) if mine else None

    def owner(self, video):
        with self.lock:
            return next((u for u, v in self.done if v == video), None)

    def forget(self, video):
        with self.lock:
            self.done = [(u, v) for u, v in self.done if v != video]

    def poll(self, client):
        with self.lock:
            pending = list(self.pending)
        still = []
        for user, video in pending:
            status, _, data = client.http("GET", f"/api/v1/videos/{video}", user=user)
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


class Worker(threading.Thread):
    """A thread with its own keep-alive connection and helpers for the gateway's API."""

    def __init__(self, stack, counts, stop, ready, seed):
        super().__init__(daemon=True)
        self.stack = stack
        self.counts = counts
        self.stop = stop
        self.ready = ready
        self.rng = random.Random(seed)
        self.conn = None

    def http(self, method, path, body=None, headers=None, user="alice", token=True):
        h = dict(headers or {})
        if token:
            h["Authorization"] = "Bearer " + self.stack.tokens[user]
        for attempt in range(2):
            if self.conn is None:
                self.conn = self.stack.connection()
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

    def create(self, size, user, name="soak.mp4"):
        body = json.dumps({"filename": name, "size_bytes": size, "content_type": "video/mp4"})
        status, _, data = self.http("POST", "/api/v1/uploads", body,
                                    {"Content-Type": "application/json"}, user=user)
        return json.loads(data) if status == 201 else None

    def patch(self, upload, offset, data, user):
        self.counts.add("chunks")
        return self.http("PATCH", f"/api/v1/uploads/{upload}", data,
                         {"Upload-Offset": str(offset)}, user=user)

    def patch_head(self, upload, total, user):
        # Every chunk request sent by hand starts here.
        self.counts.add("chunks")
        return (f"PATCH /api/v1/uploads/{upload} HTTP/1.1\r\nHost: soak\r\n"
                f"Authorization: Bearer {self.stack.tokens[user]}\r\n"
                f"Upload-Offset: 0\r\nContent-Length: {total}\r\n\r\n").encode()

    def cut_patch(self, upload, total, send, user):
        """Declares `total` bytes, sends `send` of them, and closes the socket."""
        s = self.stack.raw_socket()
        try:
            s.sendall(self.patch_head(upload, total, user))
            chunk = os.urandom(MIB)
            for _ in range(send // MIB):
                s.sendall(chunk)
        finally:
            s.close()

    def guarded(self, name, action):
        try:
            action()
        except Exception as e:  # a client error must not end the run
            self.counts.add("client_exceptions")
            log(f"{name}: {e!r}")
            if self.conn:
                self.conn.close()
                self.conn = None
        self.counts.add(f"action_{name}")


class Client(Worker):
    """One of --clients: cheap requests, a few a second."""

    def __init__(self, stack, counts, stop, ready, index):
        super().__init__(stack, counts, stop, ready, index)
        self.user = USERS[index % len(USERS)]

    def playlist(self):
        video = self.ready.pick(self.rng, self.user)
        if video is None:
            self.bad()
            return
        status, _, data = self.http("GET", f"/api/v1/videos/{video}/master.m3u8", user=self.user)
        if status != 200:
            return
        # The master names each rendition by the gateway path of its media playlist.
        rungs = [l for l in data.decode().splitlines() if l and not l.startswith("#")]
        if rungs:
            status, _, _ = self.http("GET", self.rng.choice(rungs), user=self.user)
            if status == 200:
                self.counts.add("playlists")

    def status(self):
        video = self.ready.pick(self.rng, self.user)
        self.http("GET", f"/api/v1/videos/{video or uuid.uuid4()}", user=self.user)

    def offset(self):
        self.http("HEAD", f"/api/v1/uploads/{uuid.uuid4()}", user=self.user)

    def raw(self, data):
        s = self.stack.raw_socket(timeout=30)
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
            self.http("GET", "/api/v1/nowhere", user=self.user)
        elif kind == 5:
            self.http("PUT", f"/api/v1/videos/{vid}", b"", user=self.user)
        elif kind == 6:
            up = self.create(MIB, self.user, "offset.bin")
            if up:
                self.patch(up["upload_id"], 12345, b"x" * 10, self.user)
                self.http("DELETE", f"/api/v1/uploads/{up['upload_id']}", user=self.user)
        elif kind == 7:
            self.http("POST", "/api/v1/uploads", b"{not json",
                      {"Content-Type": "application/json"}, user=self.user)
        else:
            self.raw(b"GET /healthz HTTP/1.1\r\nHost: a\r\nX-Big: " + b"a" * 20000 +
                     b"\r\n\r\n")
        self.counts.add("bad_requests")

    def run(self):
        actions = [a for a, w in MIX for _ in range(w)]
        pause = self.stack.args.pause
        while not self.stop.is_set():
            action = self.rng.choice(actions)
            self.guarded(action, getattr(self, action))
            self.stop.wait(self.rng.uniform(pause / 2, pause * 1.5))


class Uploads(Worker):
    """One upload session every 3 s: a small committed MP4, a resume or a cancellation."""

    def __init__(self, stack, counts, stop, ready, clip):
        super().__init__(stack, counts, stop, ready, 1000)
        self.clip = clip

    def upload(self):
        user = self.rng.choice(USERS)
        up = self.create(len(self.clip), user)
        if not up:
            return
        status, _, _ = self.patch(up["upload_id"], 0, self.clip, user)
        if status != 204:
            return
        status, _, _ = self.http("POST", f"/api/v1/uploads/{up['upload_id']}/commit", user=user)
        if status == 200:
            self.counts.add("uploads_committed")
            self.ready.add(user, up["video_id"])

    def resume(self):
        user = self.rng.choice(USERS)
        total = 10 * MIB
        up = self.create(total, user, "resume.bin")
        if not up:
            return
        uid = up["upload_id"]
        self.cut_patch(uid, total, 9 * MIB, user)
        offset = None
        for _ in range(100):
            status, headers, _ = self.http("HEAD", f"/api/v1/uploads/{uid}", user=user)
            if status == 204:
                offset = int(headers.get("Upload-Offset", "0"))
                break
            time.sleep(0.1)
        if offset is None:
            return
        conflicts = 0
        while offset < total:
            n = min(total - offset, up["chunk_size"])
            status, headers, _ = self.patch(uid, offset, os.urandom(n), user)
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
        self.http("DELETE", f"/api/v1/uploads/{uid}", user=user)

    def cancel(self):
        user = self.rng.choice(USERS)
        up = self.create(10 * MIB, user, "cancel.bin")
        if not up:
            return
        # Two MiB of a first chunk that will never finish: the store holds nothing durable,
        # and the discard must still release everything the gateway opened for it.
        self.cut_patch(up["upload_id"], 10 * MIB, 2 * MIB, user)
        status, _, _ = self.http("DELETE", f"/api/v1/uploads/{up['upload_id']}", user=user)
        if status == 204:
            self.counts.add("cancels")

    def run(self):
        # One session in four commits a video: one every 12 s, which the worker transcodes in
        # under a second, and 6 h of which, raw and HLS, take ~0.5 GB of MinIO's tmpfs.
        sessions = ["upload", "resume", "cancel", "resume"]
        i = 0
        while not self.stop.is_set():
            name = sessions[i % len(sessions)]
            self.guarded(name, getattr(self, name))
            self.counts.add("upload_sessions")
            i += 1
            self.stop.wait(3.0)


class Slow(Worker):
    """Every 20 s a client that one of the gateway's timers has to end."""

    def __init__(self, stack, counts, stop, ready):
        super().__init__(stack, counts, stop, ready, 2000)

    def await_end(self, s, name):
        # The gateway answers 408 or just closes; either way the socket ends.
        try:
            while s.recv(4096):
                pass
            self.counts.add(f"slow_{name}_ended")
        except OSError:
            self.counts.add(f"slow_{name}_errors")
        finally:
            s.close()

    def header(self):
        s = self.stack.raw_socket()
        s.sendall(b"GET /healthz HTTP/1.1\r\nHost: soak\r\n")
        self.await_end(s, "header")

    def body(self, drip):
        user = SLOW_USER
        conn = Worker(self.stack, self.counts, self.stop, self.ready, 0)
        up = conn.create(MIB, user, "slow.bin")
        if not up:
            return
        s = self.stack.raw_socket()
        s.sendall(conn.patch_head(up["upload_id"], MIB, user) + b"x" * 1024)
        if drip:
            # 100 bytes every 2 s, 50 B/s: far below the 8 KiB/s floor over its 30 s window.
            try:
                for _ in range(40):
                    if self.stop.wait(2.0):
                        break
                    s.sendall(b"y" * 100)
            except OSError:
                pass
        self.await_end(s, "rate" if drip else "idle")
        conn.http("DELETE", f"/api/v1/uploads/{up['upload_id']}", user=user)

    def run(self):
        kinds = [("header", self.header), ("idle", lambda: self.body(False)),
                 ("rate", lambda: self.body(True))]
        i = 0
        while not self.stop.wait(20.0):
            name, action = kinds[i % len(kinds)]
            threading.Thread(target=self.guarded, args=(f"slow_{name}", action),
                             daemon=True).start()
            i += 1


class Saturation(Worker):
    """Every 60 s: fills every upload slot, then asks for more."""

    def __init__(self, stack, counts, stop, ready):
        super().__init__(stack, counts, stop, ready, 3000)

    def hold(self, user, upload):
        s = self.stack.raw_socket()
        s.sendall(self.patch_head(upload, MIB, user) + b"x" * 1024)
        return s

    def ask(self, user, upload, what):
        status, _, _ = self.patch(upload, 0, b"x" * 1024, user)
        self.counts.add(f"saturation_{what}_{status}")

    def saturate(self):
        uploads = {u: [self.create(MIB, u, "hold.bin") for _ in range(4)] for u in HOLDERS}
        held = []
        try:
            if any(up is None for ups in uploads.values() for up in ups):
                return
            first, second = HOLDERS[0], HOLDERS[1]
            held += [self.hold(first, up["upload_id"]) for up in uploads[first][:3]]
            # The holds' heads have to reach the gateway before the question does.
            time.sleep(0.5)
            self.ask(first, uploads[first][3]["upload_id"], "user")
            for user in HOLDERS[1:]:
                held += [self.hold(user, up["upload_id"]) for up in uploads[user][:3]]
            time.sleep(0.5)
            self.ask(second, uploads[second][3]["upload_id"], "total")
        finally:
            for s in held:
                s.close()
            for user, ups in uploads.items():
                for up in ups:
                    if up:
                        self.http("DELETE", f"/api/v1/uploads/{up['upload_id']}", user=user)

    def run(self):
        while not self.stop.wait(60.0):
            self.guarded("saturation", self.saturate)


class StoreFaults(Worker):
    """Every 120 s a ready video loses its media playlist in the store, then is played."""

    def __init__(self, stack, counts, stop, ready):
        super().__init__(stack, counts, stop, ready, 4000)

    def break_one(self):
        video = self.ready.pick(self.rng)
        if video is None:
            return
        user = self.ready.owner(video)
        self.ready.forget(video)
        status, _, data = self.http("GET", f"/api/v1/videos/{video}/master.m3u8", user=user)
        if status != 200:
            return
        rungs = [l for l in data.decode().splitlines() if l and not l.startswith("#")]
        if not rungs:
            return
        rung = rungs[0].rstrip("/").split("/")[-2]
        self.stack.s3("DELETE", f"{self.stack.bucket}/videos/{video}/hls/{rung}/index.m3u8")
        status, _, _ = self.http("GET", rungs[0], user=user)
        self.counts.add(f"store_missing_{status}")

    def run(self):
        while not self.stop.wait(120.0):
            self.guarded("store_fault", self.break_one)


def proc_sample(pid):
    rss = None
    with open(f"/proc/{pid}/status") as f:
        for line in f:
            if line.startswith("VmRSS:"):
                rss = int(line.split()[1])
    fds = len(os.listdir(f"/proc/{pid}/fd"))
    return rss, fds


def fit(points):
    """Least-squares slope per hour of (hours, value) points, and its standard error."""
    n = len(points)
    mx = sum(x for x, _ in points) / n
    my = sum(y for _, y in points) / n
    sxx = sum((x - mx) ** 2 for x, _ in points)
    if sxx == 0 or n < 3:
        return 0.0, float("inf")
    slope = sum((x - mx) * (y - my) for x, y in points) / sxx
    residual = sum((y - (my + slope * (x - mx))) ** 2 for x, y in points)
    return slope, math.sqrt(residual / (n - 2) / sxx)


def per_hour(window, key):
    hours = (window[-1]["elapsed_min"] - window[0]["elapsed_min"]) / 60
    return (window[-1][key] - window[0][key]) / hours if hours > 0 else 0.0


def split(rows):
    return ([r for r in rows if r["elapsed_min"] < WARMUP_MINUTES],
            [r for r in rows if r["elapsed_min"] >= WARMUP_MINUTES])


def judge(rows, clients):
    """Returns (passed, report lines) for the samples in `rows`."""
    warm, window = split(rows)
    if len(window) < 10:
        return False, ["too few samples after the warm-up to judge"]
    report = []
    passed = True
    hours = (window[-1]["elapsed_min"] - window[0]["elapsed_min"]) / 60
    rates = {"request": per_hour(window, "requests"),
             "chunk request": per_hour(window, "chunks") if "chunks" in window[0] else None,
             "upload session": per_hour(window, "upload_sessions"),
             "job": per_hour(window, "uploads_committed")}
    report.append(f"window: {len(window)} samples over {hours:.2f} h after a "
                  f"{WARMUP_MINUTES} min warm-up; per hour: " +
                  ", ".join(f"{v:.0f} {k}s" for k, v in rates.items() if v is not None))
    units = {"gateway": [("request", PRODUCTION_REQUESTS_PER_S),
                         ("chunk request", PRODUCTION_REQUESTS_PER_S),
                         ("upload session", PRODUCTION_UPLOADS_PER_S)],
             "worker": [("job", PRODUCTION_JOBS_PER_S)]}
    for proc in ["gateway", "worker"]:
        rss = [(r["elapsed_min"] / 60, r[f"{proc}_rss_kb"] * 1024) for r in window]
        peak = max(v for _, v in rss)
        slope, se = fit(rss)
        upper = slope + Z95 * se
        report.append(f"{proc} rss: first {rss[0][1] / MIB:.1f} MiB, last {rss[-1][1] / MIB:.1f} "
                      f"MiB, peak {peak / MIB:.1f} MiB, slope {slope / 1e3:+.1f} KB/h, 95% "
                      f"upper end {upper / 1e3:+.1f} KB/h")
        for unit, production in units[proc]:
            bound = (MEMORY_HIGH[proc] - peak) / (production * MONTH_SECONDS)
            if rates[unit] is None:
                # Runs from before the column; the per-session bound is what covers chunks there.
                report.append(f"  per {unit}: not recorded by this run: not judged")
                continue
            if rates[unit] <= 0:
                report.append(f"  per {unit}: none in the window: NOT JUDGED")
                passed = False
                continue
            leak = upper / rates[unit]
            ok = leak < bound
            passed = passed and ok
            report.append(f"  per {unit}: at most {leak:.3f} B, bound {bound:.3f} B: "
                          f"{'flat' if ok else 'NOT SHOWN FLAT'}")
        fds = [(r["elapsed_min"] / 60, r[f"{proc}_fds"]) for r in window]
        fd_slope, fd_se = fit(fds)
        fd_rise = (fd_slope + Z95 * fd_se) * hours
        warm_fds = [r[f"{proc}_fds"] for r in warm] or [0]
        in_flight = (2 * (clients + 1) + SLOW_MAX + UPLOAD_SLOTS + 1 if proc == "gateway"
                     else WORKER_FD_NOISE)
        fd_bound = max(in_flight, max(warm_fds) - min(warm_fds))
        fd_ok = fd_rise < fd_bound
        passed = passed and fd_ok
        report.append(f"{proc} fds: first {fds[0][1]}, last {fds[-1][1]}, min "
                      f"{min(v for _, v in fds)}, max {max(v for _, v in fds)}, rise over the "
                      f"window at most {fd_rise:+.2f}, bound {fd_bound}: "
                      f"{'flat' if fd_ok else 'NOT FLAT'}")
    return passed, report


def judge_hourly(rows, clients):
    """The criterion judge() replaced: RSS slope per hour, not normalised, against MemoryHigh
    over 30 days; the fitted fd rise against 2 per client. Kept to read earlier runs by."""
    warm, window = split(rows)
    if len(window) < 10:
        return False, ["too few samples after the warm-up to judge"]
    hours = (window[-1]["elapsed_min"] - window[0]["elapsed_min"]) / 60
    passed = True
    report = []
    for proc in ["gateway", "worker"]:
        rss = [(r["elapsed_min"] / 60, r[f"{proc}_rss_kb"] * 1024) for r in window]
        peak = max(v for _, v in rss)
        slope, _ = fit(rss)
        bound = (MEMORY_HIGH[proc] - peak) / (MONTH_SECONDS / 3600)
        fd_slope, _ = fit([(r["elapsed_min"] / 60, r[f"{proc}_fds"]) for r in window])
        warm_fds = [r[f"{proc}_fds"] for r in warm] or [0]
        fd_bound = max(2 * clients if proc == "gateway" else WORKER_FD_NOISE,
                       max(warm_fds) - min(warm_fds))
        ok = slope < bound and fd_slope * hours < fd_bound
        passed = passed and ok
        report.append(f"{proc}: rss slope {slope / 1e6:+.3f} MB/h, bound {bound / 1e6:.3f} MB/h; "
                      f"fd rise {fd_slope * hours:+.2f}, bound {fd_bound}: "
                      f"{'flat' if ok else 'NOT FLAT'}")
    return passed, report


def load_samples(path):
    rows = []
    with open(path) as f:
        for r in csv.DictReader(f):
            row = {k: (float(v) if k == "elapsed_min" else (int(v) if v.isdigit() else v))
                   for k, v in r.items()}
            # Runs from before these columns: the same quantities from what they recorded.
            if "requests" not in row:
                row["requests"] = row["status_2xx"] + row["status_4xx"] + row["status_5xx"]
            if "upload_sessions" not in row:
                row["upload_sessions"] = (row["uploads_committed"] + row["resumes_completed"] +
                                          row["cancels"])
            rows.append(row)
    return rows


def rejudge(path, clients):
    rows = load_samples(path)
    new, new_report = judge(rows, clients)
    old, old_report = judge_hourly(rows, clients)
    print(f"{path}: {len(rows)} samples, judged for {clients} clients")
    print("per-unit criterion (current):")
    print("\n".join("  " + line for line in new_report))
    print(f"  verdict: {'PASS' if new else 'FAIL'}")
    print("hourly criterion (replaced):")
    print("\n".join("  " + line for line in old_report))
    print(f"  verdict: {'PASS' if old else 'FAIL'}")
    return 0 if new else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--build", default=str(ROOT / "build" / "ci"))
    parser.add_argument("--hours", type=float, default=6.0)
    parser.add_argument("--out")
    parser.add_argument("--port", type=int, default=18180)
    parser.add_argument("--clients", type=int, default=8)
    parser.add_argument("--pause", type=float, default=0.3,
                        help="mean seconds between one client's actions")
    parser.add_argument("--interval", type=float, default=60.0, help="seconds between samples")
    parser.add_argument("--rejudge", metavar="SAMPLES_CSV")
    args = parser.parse_args()
    if args.rejudge:
        return rejudge(args.rejudge, args.clients)
    if not args.out:
        parser.error("--out is required for a run")

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
    # Whatever start() launched before it failed is stopped: the soak may run as root, where
    # nothing else would stop it.
    try:
        stack.start()
    except BaseException:
        stack.stop()
        raise
    pids = stack.pids()
    (out / "pids.json").write_text(json.dumps({**pids, "soak": os.getpid()}))
    log(f"started: gateway pid {pids['gateway']}, worker pid {pids['worker']}, "
        f"database {stack.name}, bucket {stack.bucket}")

    stop = threading.Event()
    counts = Counts()
    ready = ReadyVideos()
    threads = [Client(stack, counts, stop, ready, i) for i in range(args.clients)]
    threads += [Uploads(stack, counts, stop, ready, clip.read_bytes()),
                Slow(stack, counts, stop, ready), Saturation(stack, counts, stop, ready),
                StoreFaults(stack, counts, stop, ready)]
    for t in threads:
        t.start()
    poller = Worker(stack, Counts(), stop, ready, 99)

    rows = []
    fields = ["time", "elapsed_min", "gateway_rss_kb", "gateway_fds", "worker_rss_kb",
              "worker_fds", "requests", "status_2xx", "status_4xx", "status_5xx",
              "transport_errors", "chunks", "upload_sessions", "uploads_committed",
              "videos_ready",
              "resumes_completed", "cancels", "playlists", "sighups"]
    started = time.time()
    end = started + args.hours * 3600
    next_sighup = started + 600
    sighups = 0
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
            if time.time() >= next_sighup:
                stack.procs["gateway"].send_signal(signal.SIGHUP)
                sighups += 1
                next_sighup += 600
            if time.time() >= next_sample:
                g = proc_sample(pids["gateway"])
                w = proc_sample(pids["worker"])
                c = counts.snapshot()
                statuses = {k: c.get(k, 0) for k in ["status_2xx", "status_4xx", "status_5xx"]}
                row = {"time": time.strftime("%Y-%m-%dT%H:%M:%S"),
                       "elapsed_min": round((time.time() - started) / 60, 2),
                       "gateway_rss_kb": g[0], "gateway_fds": g[1],
                       "worker_rss_kb": w[0], "worker_fds": w[1],
                       "requests": sum(statuses.values()), **statuses,
                       "transport_errors": c.get("transport_errors", 0),
                       "chunks": c.get("chunks", 0),
                       "upload_sessions": c.get("upload_sessions", 0),
                       "uploads_committed": c.get("uploads_committed", 0),
                       # Capped at the working set of 200.
                       "videos_ready": len(ready.done),
                       "resumes_completed": c.get("resumes_completed", 0),
                       "cancels": c.get("cancels", 0),
                       "playlists": c.get("playlists", 0),
                       "sighups": sighups}
                writer.writerow(row)
                f.flush()
                rows.append(row)
                next_sample += args.interval
            time.sleep(min(5.0, max(0.0, next_sample - time.time())))

    stop.set()
    for t in threads:
        t.join(timeout=120)
    codes = stack.stop()
    stack.cleanup()
    passed, report = judge(rows, args.clients)
    summary = [f"soak: {args.hours} h requested, {len(rows)} samples, {args.clients} clients",
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
