#!/usr/bin/env python3
"""Soak test: three chat_server nodes on one Postgres under a steady mixed load for hours.

Starts the cluster the M16 acceptance runs (three processes, one database, loopback node
channel), drives WebSocket clients through all three nodes, samples each node's resident memory,
open descriptors and /metrics every minute into samples.csv, and judges at the end whether every
node stayed flat, by the criterion of ADR-0042 applied to chat's units of work (below).

    tests/soak/chat_soak.py --build build/ci --hours 6 --out /some/dir
    tests/soak/chat_soak.py --self-test --out /some/dir           # 12 minutes, proves the harness
    tests/soak/chat_soak.py --rejudge /some/dir/samples.csv --clients 64

The database is created in the server named by ULW_TEST_DATABASE_URL (the shared Postgres of
deploy/local/compose.yaml by default) and dropped at the end. The binaries are copied into
<out>/bin first. Exit status: 0 flat, 1 not flat or a path of the mix never ran, 2 the run
itself failed.

The load (--clients and --pause scale it):
  clients     --clients threads, each one user with one connection at a time to a node picked at
              random. A connection lives 1 to 5 minutes, joins ROOMS_PER_CLIENT of the ROOM_POOL
              rooms (one of them lossy for every fourth client) resuming each from its last seq,
              and every --pause seconds or so sends a message (base64url, 16 to 600 bytes), sends
              the last one again with its id (deduplicated), pages history, watches or unwatches
              a user, pings, or sends a bad command (not JSON, unknown type, bad room, bad id, bad
              body, a room not joined); then it ends with a Close or a reset, and reconnects
  visitors    one short connection a second: joins one or two rooms (a pool room resumed from a
              seq, or a new room that the nodes forget once it lingers out), maybe sends one
              message, reads for a few seconds and leaves; one in ten is a refused upgrade (no
              token, a forged one, a wrong path, a GET that is not an upgrade)
  burst       every 30 s one user sends 16 messages at once: the send bucket's 10 and then
              rate_limited
  slow        every 60 s, for up to 150 s, in turn: a consumer that stops reading (the kernel
              ends it at the nodes' 20 s TCP_USER_TIMEOUT), a durable one reading SLOW_READ a
              second (its node closes it as a slow consumer) and a lossy one reading as much (its
              node skips it); the last two while a firehose sends many times that into a room of
              theirs. Each also resumes two pool rooms from seq 0, and pings every 20 s so that it
              is not closed as idle first
  owners      every 10 minutes one node, in turn, is stopped (SIGSTOP) for longer than a room
              owner's heartbeat may lapse, so the other nodes take its rooms under a higher
              generation, and then continued (SIGCONT): its writes in flight are fenced out
  SIGHUP      every 10 minutes, between the owner changes, to every node; chat_server takes it
              and reloads nothing today (it serves plain WebSocket behind the edge's TLS)
  prefill     in the warm-up only: large frames that fill io_uring's receive pool, and 60
              more senders in the pool rooms that bring each node's order of kept messages to
              its cap (PREFILL_FRAME, PREFILL_SENDERS, ADR-0069)
Where the database has member lists (M19), every user of the soak is listed in the pool rooms,
and the visitors in the rooms they open, as an operator would list them: a group chat admits
only its members. Commands the running chat_server does not know are found at the start by
trying each one and reading `malformed` as absent: `history` (M19) and `watch`/`unwatch` (M18).
What is absent is left out of the mix and named in the summary.

Every connection comes from its own address in 127/8 (loopback_source()), and every node runs
without transparent huge pages (without_huge_pages()).

Not driven: the JWKS fetch path (the nodes verify against a local key set), database outages,
and a node restart (a new process would start a new RSS series; ownership changes come from
SIGSTOP instead).

Flatness, judge(): as ADR-0042. The first WARMUP_MINUTES are excluded; a least-squares line is
fitted to each series over the rest, and the upper end of its 95% confidence interval is judged,
so a run too short or too quiet to tell fails rather than passes.

  RSS   Per unit of work, against the memory a node may still take:
            leak per unit < (MEMORY_LIMIT - peak) / (production rate x 720 h)
        MEMORY_LIMIT is the chat pod's 1 GiB (ADR-0036, ADR-0043); chat has no systemd unit and so
        no MemoryHigh below it. Production rates per node are ceilings, from the limits the
        service enforces (apps/chat/src/chat.hpp, chat_service.hpp):
          commands     3840/s: 1280 connections, each its own user at the sustained 2 sends and
                       1 join or history page a second; commands refused at parse are few
                       beside them. About 0.10 bytes per command.
          deliveries   25600/s: those users' 2560 sends a second, each delivered to the ten
                       members of a room, ADR-0012's larger room. About 0.016 bytes per delivery.
                       Judged on their own, as ADR-0042 judges chunk requests: a leak on each
                       delivery is per subscriber, and per command it would be read against a
                       fan-out the soak chose.
          connections  42.7/s: all 1280 connections reconnecting every 30 s, the linger the
                       resume path is sized for (ADR-0043). About 9.5 bytes per connection.
  fds   Any sustained rise is a leak. A sample may legitimately differ by the connections the
        soak can hold on one node at once: every client, the visitors in flight, the slow
        consumers, the burst connection, the firehose and a refused upgrade (fd_bound). The
        upper end of the fitted rise over the window must stay under that, or the range the
        warm-up showed if larger.

--self-test runs 12 minutes with a 3 minute warm-up, samples every 15 s and changes owners
every 150 s. It passes when every path of the mix ran, each node served, and every node exited 0
on SIGTERM; its flatness is printed but is not a verdict, since 9 minutes cannot resolve bounds
set for 6 hours.
"""

import argparse
import base64
import csv
import ctypes
import http.client
import json
import os
import random
import secrets
import shutil
import signal
import socket
import struct
import subprocess
import sys
import threading
import time
import traceback
import uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from soak import MONTH_SECONDS, Z95, Counts, env_or, fit, log, per_hour, proc_sample  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
MIB = 1024 * 1024
WARMUP_MINUTES = 15
MEMORY_LIMIT = 1024 * MIB
# 1280 connections (chat::Limits::max_connections), each its own user.
PRODUCTION_CONNECTIONS = 1280
PRODUCTION_COMMANDS_PER_S = PRODUCTION_CONNECTIONS * (2 + 1)
PRODUCTION_DELIVERIES_PER_S = PRODUCTION_CONNECTIONS * 2 * 10
PRODUCTION_UPGRADES_PER_S = PRODUCTION_CONNECTIONS / 30
UNITS = [("command", "commands", PRODUCTION_COMMANDS_PER_S),
         ("delivery", "deliveries", PRODUCTION_DELIVERIES_PER_S),
         ("connection", "upgrades", PRODUCTION_UPGRADES_PER_S)]
NODES = ["chat-1", "chat-2", "chat-3"]
ISSUER = "https://auth.soak.test"
ROOM_POOL = 24
ROOMS_PER_CLIENT = 6
VISITORS = 16
# A visitor lasts at most ~4 s and one starts a second; more than this at once are skipped.
VISITORS_IN_FLIGHT = 6
# Slow consumers last 150 s and one starts every 60 s.
SLOW_MAX = 3
# Clients announce Ethernet's MSS (1500 less 40 of headers and 12 of timestamps), not loopback's
# 64 KiB: the kernel sizes a connection's send buffer from ten segments of it, and at 64 KiB it
# would take over a megabyte that a real client's connection never gets before a node's own
# queue sees any of it.
ETHERNET_MSS = 1448
# A slow consumer reads 8 KiB a second through a 16 KiB receive buffer, so its window keeps
# opening inside the nodes' 20 s TCP_USER_TIMEOUT, which ends a consumer that stops reading (the
# stalled kind shows that). The firehose sends it 32 bodies of 32 KiB, two a second (the send
# limit): about 1.4 MB as base64url, past the ~450 KB the kernel holds for it and the 256 KiB of
# the node's own queue that closes a durable consumer, within about 10 s.
SLOW_READ = 8 * 1024
SLOW_RCVBUF = 16 * 1024
HOSE_BODY = 32 * 1024
HOSE_MESSAGES = 32
# The warm-up prefill (ADR-0069). Two structures of each node grow to a fixed size more slowly
# than the warm-up lasts at this load, and a line fitted after the warm-up would read their
# last climb as a leak:
#  - io_uring's receive pool, 256 buffers of 64 KiB whose pages count in RSS once the kernel has
#    written into them: PREFILL_FRAMES frames of PREFILL_FRAME bytes that are not JSON (answered
#    not_json, never sequenced or stored), over loopback's 64 KiB MSS. They go in writes of
#    PREFILL_BATCH frames (480 KiB), so the socket holds more than a buffer whenever the node
#    reads and every read but a write's last fills a whole 64 KiB buffer; about 1,900 of them go
#    round the ring more than seven times. Frames one at a time, answered between, left a read
#    to find less than a buffer waiting, and some pages untouched until after the warm-up;
#  - the chat service's order of kept messages, 131,072 entries (about 3 MB) at its cap, of which
#    the soak's own ~60 messages a second into each node fill about half in the warm-up:
#    PREFILL_SENDERS more, two messages a second each (the send limit), into the pool rooms that
#    every node is already in, so each message counts on all three nodes. The pool rooms are
#    at their own 256 KiB already, so this keeps nothing more than the load does: no room is
#    made that would later free its memory, and the bodies are drawn as the clients' are.
# Both stop PREFILL_MARGIN_MINUTES before the warm-up ends.
PREFILL_FRAME = 60 * 1024
PREFILL_FRAMES = 2048
PREFILL_BATCH = 8
PREFILL_SENDERS = 60
PREFILL_MARGIN_MINUTES = 2
# Longer than rt::kOwnerStaleAfter (5 s), so another node claims the stopped node's rooms.
OWNER_STOP_S = 8.0
MIX = [("send", 70), ("resend", 6), ("history", 8), ("watch", 6), ("ping", 5), ("bad", 5)]
METRICS = {"commands": "messages_received_total", "deliveries": "messages_delivered_total",
           "upgrades": "websocket_upgrades_total", "connections": "connections_current",
           "rooms": "rooms_joined", "rooms_owned": "rooms_active",
           "reassignments": "room_reassignments_total", "fenced": "fenced_writes_total",
           "rate_limited": "messages_rate_limited_total",
           "deduplicated": "messages_deduplicated_total", "replayed": "messages_replayed_total",
           "kept_bytes": "messages_kept_bytes", "lossy_drops": "lossy_drops_total",
           "slow_consumers": "slow_consumers_total", "forwards": "forwards_total",
           "peers_lost": "peers_lost_total", "allocation_failures": "allocation_failures_total",
           # Zero on a server without history (M19) or presence (M18).
           "history_messages": "history_messages_total", "presence_rooms": "presence_rooms",
           "presence_sent": "presence_events_sent_total",
           "presence_notified": "presence_notifications_total"}
DRIVER = ["sends", "acks", "messages_in", "resumes", "history_pages", "presence",
          "owner_changes", "sighups", "transport_errors"]


def column(node):
    return node.replace("-", "")


def fd_bound(clients):
    # The burst connection, the firehose and a refused upgrade are the last three.
    return clients + VISITORS_IN_FLIGHT + SLOW_MAX + 3


def without_huge_pages():
    """Runs in each node's process before exec: PR_SET_THP_DISABLE (41) holds across execve. RSS
    is the leak signal here, and where transparent huge pages are "always" (GitHub's runners)
    khugepaged can collapse a partly touched 2 MiB range and add the rest of it to RSS with
    nothing allocated (#50)."""
    if ctypes.CDLL(None, use_errno=True).prctl(41, 1, 0, 0, 0) != 0:
        raise OSError(ctypes.get_errno(), "PR_SET_THP_DISABLE")


def loopback_source():
    """A source address anywhere in 127/8. From 127.0.0.1 alone, new connections keep drawing
    ports whose 4-tuples a node still holds in TIME_WAIT (it closes first after a Close), and
    such a SYN can cost its client a retransmission a second later: the soak's artefact, not
    the server's."""
    return f"127.{random.randrange(1, 255)}.{random.randrange(256)}.{random.randrange(1, 255)}"


class Refused(Exception):
    def __init__(self, status):
        super().__init__(f"upgrade refused: HTTP {status}")
        self.status = status


class Closed(Exception):
    pass


class Ws:
    """A blocking RFC 6455 client: masked text, ping and close frames out; unmasked frames in,
    Pings answered as they are read."""

    def __init__(self, sock, rest):
        self.sock = sock
        self.buf = bytearray(rest)

    @classmethod
    def connect(cls, port, token=None, path="/rt", upgrade=True, rcvbuf=None, mss=ETHERNET_MSS):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            if rcvbuf:
                # Before connect, or the window is already advertised.
                s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
            s.bind((loopback_source(), 0))
            if mss:
                s.setsockopt(socket.IPPROTO_TCP, socket.TCP_MAXSEG, mss)
            s.settimeout(5)
            s.connect(("127.0.0.1", port))
            head = f"GET {path} HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\n"
            if upgrade:
                key = base64.b64encode(os.urandom(16)).decode()
                head += ("Upgrade: websocket\r\nConnection: Upgrade\r\n"
                         f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n")
            if token:
                head += f"Authorization: Bearer {token}\r\n"
            s.sendall((head + "\r\n").encode())
            data = b""
            while b"\r\n\r\n" not in data:
                chunk = s.recv(4096)
                if not chunk:
                    raise Refused(0)
                data += chunk
            status_line, _, rest = data.partition(b"\r\n\r\n")
            status = int(status_line.split(b" ", 2)[1])
            if status != 101:
                raise Refused(status)
            return cls(s, rest)
        except BaseException:
            s.close()
            raise

    def send(self, opcode, payload):
        self.sock.sendall(self.frame_bytes(opcode, payload))

    @staticmethod
    def frame_bytes(opcode, payload):
        n = len(payload)
        head = bytes([0x80 | opcode])
        if n < 126:
            head += bytes([0x80 | n])
        elif n < 65536:
            head += bytes([0x80 | 126]) + n.to_bytes(2, "big")
        else:
            head += bytes([0x80 | 127]) + n.to_bytes(8, "big")
        mask = os.urandom(4)
        masked = b""
        if n:
            key = (mask * (n // 4 + 1))[:n]
            masked = (int.from_bytes(payload, "big") ^ int.from_bytes(key, "big")).to_bytes(
                n, "big")
        return head + mask + masked

    def send_json(self, obj):
        self.send(0x1, json.dumps(obj, separators=(",", ":")).encode())

    def frame(self):
        b = self.buf
        if len(b) < 2:
            return None
        n, at = b[1] & 0x7F, 2
        if n == 126:
            if len(b) < 4:
                return None
            n, at = int.from_bytes(b[2:4], "big"), 4
        elif n == 127:
            if len(b) < 10:
                return None
            n, at = int.from_bytes(b[2:10], "big"), 10
        if len(b) < at + n:
            return None
        op, payload = b[0] & 0x0F, bytes(b[at:at + n])
        del b[:at + n]
        return op, payload

    def recv(self, timeout):
        """The next text message, or None when `timeout` passes first. Raises Closed."""
        deadline = time.monotonic() + timeout
        while True:
            f = self.frame()
            if f:
                op, payload = f
                if op == 0x9:
                    self.send(0xA, payload)
                elif op == 0x8:
                    raise Closed(int.from_bytes(payload[:2], "big") if len(payload) >= 2 else 0)
                elif op == 0x1:
                    return payload.decode()
                continue
            left = deadline - time.monotonic()
            if left <= 0:
                return None
            self.sock.settimeout(left)
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                return None
            if not chunk:
                raise Closed(0)
            self.buf += chunk

    def close(self, clean):
        try:
            if clean:
                self.send(0x8, (1000).to_bytes(2, "big"))
                try:
                    while self.recv(2.0) is not None:
                        pass
                except Closed:
                    pass
            else:
                # A reset: the peer vanishes without a Close, as a phone losing its network.
                self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        except OSError:
            pass
        finally:
            self.sock.close()


def unexpected(counts, who, e):
    counts.add("soak_exceptions")
    frames = traceback.extract_tb(e.__traceback__)
    log(f"{who}: {e!r} at " + ", ".join(f"{f.name}:{f.lineno}" for f in frames[-3:]))


def b64(data):
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()


class Features:
    def __init__(self):
        self.members = False
        self.history = False
        self.presence = False

    def absent(self):
        return [name for name, on in [("member lists (M19)", self.members),
                                      ("history (M19)", self.history),
                                      ("watch and unwatch (M18)", self.presence)] if not on]


class Stack:
    """The database, keys, and the three chat_server processes."""

    def __init__(self, args, out):
        self.args = args
        self.out = out
        self.bin = out / "bin"
        self.name = "ulw_chatsoak_" + time.strftime("%Y%m%d%H%M%S") + "_" + uuid.uuid4().hex[:6]
        self.admin_url = env_or("ULW_TEST_DATABASE_URL",
                                "postgresql://postgres:testtest123@127.0.0.1:55432/postgres")
        self.database_url = self.admin_url.rsplit("/", 1)[0] + "/" + self.name
        self.ports = {n: args.port + i for i, n in enumerate(NODES)}
        self.node_ports = {n: args.port + 100 + i for i, n in enumerate(NODES)}
        self.secret = secrets.token_hex(32)
        self.procs = {}
        self.tokens = {}
        self.features = Features()

    def copy_binaries(self):
        self.bin.mkdir(parents=True, exist_ok=True)
        build = Path(self.args.build).resolve()
        for rel in ["apps/chat/chat_server", "apps/migrate/ulw_migrate",
                    "tools/devtoken/ulw_devtoken"]:
            shutil.copy2(build / rel, self.bin / Path(rel).name)

    def prepare(self, users):
        subprocess.run(["psql", self.admin_url, "-qc", f"CREATE DATABASE {self.name}"],
                       check=True)
        subprocess.run([self.bin / "ulw_migrate"], check=True,
                       env={**os.environ, "ULW_DATABASE_URL": self.database_url,
                            "ULW_ALLOW_ROOT": "1"},
                       stdout=subprocess.DEVNULL)
        self.features.members = subprocess.run(
            ["psql", self.database_url, "-Atc", "SELECT to_regclass('chat_members') IS NOT NULL"],
            check=True, capture_output=True, text=True).stdout.strip() == "t"
        key = self.out / "dev-key.json"
        subprocess.run([self.bin / "ulw_devtoken", "keygen", key], check=True)
        jwks = subprocess.run([self.bin / "ulw_devtoken", "jwks", key], check=True,
                              capture_output=True, text=True).stdout
        (self.out / "jwks.json").write_text(jwks)
        for user in users:
            self.tokens[user] = subprocess.run(
                [self.bin / "ulw_devtoken", "mint", key, "--iss", ISSUER, "--sub", user,
                 "--ttl", str(7 * 24 * 3600)],
                check=True, capture_output=True, text=True).stdout.strip()

    def start(self):
        for node in NODES:
            env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"),
                   "ULW_NODE_ID": node,
                   "ULW_LISTEN_PORT": str(self.ports[node]),
                   "ULW_NODE_ADDRESS": f"127.0.0.1:{self.node_ports[node]}",
                   "ULW_DEV_LOOPBACK_NODES": "1",
                   "ULW_NODE_SECRET": self.secret,
                   "ULW_DATABASE_URL": self.database_url,
                   "ULW_DEV_JWKS_FILE": str(self.out / "jwks.json"),
                   "ULW_DEV_MODE": "1",
                   "JWT_ISSUER": ISSUER,
                   # The soak may run as root on a development host.
                   "ULW_ALLOW_ROOT": "1",
                   # Every client comes from this host's one address (ADR-0076's limits).
                   "ULW_MAX_CONNECTIONS_PER_IP": "1280",
                   "ULW_NEW_CONNECTIONS_PER_IP_PER_SECOND": "65536"}
            if os.environ.get("ULW_REACTOR"):
                env["ULW_REACTOR"] = os.environ["ULW_REACTOR"]
            # Experiments only: an allocator to put in front of glibc's.
            if os.environ.get("ULW_SOAK_PRELOAD"):
                env["LD_PRELOAD"] = os.environ["ULW_SOAK_PRELOAD"]
            out = open(self.out / f"{node}.log", "ab")
            self.procs[node] = subprocess.Popen([self.bin / "chat_server"], env=env, stdout=out,
                                                stderr=subprocess.STDOUT,
                                                preexec_fn=without_huge_pages)
        deadline = time.time() + 60
        for node in NODES:
            while True:
                if self.get(node, "/readyz")[0] == 200:
                    break
                if time.time() > deadline:
                    raise RuntimeError(f"{node} never became ready; see {node}.log")
                time.sleep(0.5)

    def get(self, node, path, timeout=5):
        for _ in range(2):
            try:
                c = http.client.HTTPConnection("127.0.0.1", self.ports[node], timeout=timeout,
                                               source_address=(loopback_source(), 0))
                c.request("GET", path)
                r = c.getresponse()
                return r.status, r.read().decode()
            except (OSError, http.client.HTTPException):
                pass
        return 0, ""

    def metrics(self, node):
        status, text = self.get(node, "/metrics")
        if status != 200:
            return None
        out = {}
        for line in text.splitlines():
            name, _, value = line.rpartition(" ")
            if name and value.isdigit():
                out[name] = int(value)
        return out

    def grant(self, rooms, users):
        """Lists `users` as members of `rooms`, as an operator would (M19: a group chat admits
        only its members). A server without member lists has no table for them."""
        if not self.features.members:
            return
        sql = ("INSERT INTO chat_members (room_id, user_id) SELECT r, u FROM "
               f"unnest('{{{','.join(rooms)}}}'::uuid[]) r CROSS JOIN "
               f"unnest('{{{','.join(users)}}}'::text[]) u ON CONFLICT DO NOTHING;\n")
        subprocess.run(["psql", self.database_url, "-q", "-v", "ON_ERROR_STOP=1"], input=sql,
                       text=True, check=True, stdout=subprocess.DEVNULL)

    def detect(self, user, other, room):
        """Tries each command main may not have; `malformed` means this server lacks it, any
        other answer that it has it. `room` must admit `user`."""
        f = self.features
        ws = Ws.connect(self.ports[NODES[0]], self.tokens[user])

        def answer():
            while True:
                text = ws.recv(10.0)
                if text is None:
                    raise RuntimeError("no answer while detecting features")
                m = json.loads(text)
                if m["type"] != "message":
                    return m

        def known():
            m = answer()
            return not (m["type"] == "error" and m.get("reason") == "malformed")

        try:
            ws.send_json({"type": "join", "room": room})
            m = answer()
            if m["type"] != "joined":
                raise RuntimeError(f"feature probe could not join: {m}")
            ws.send_json({"type": "history", "room": room, "limit": 1})
            f.history = known()
            ws.send_json({"type": "watch", "user": other})
            f.presence = known()
            if f.presence:
                ws.send_json({"type": "unwatch", "user": other})
        finally:
            ws.close(clean=True)

    def pids(self):
        return {n: p.pid for n, p in self.procs.items()}

    def alive(self):
        return all(p.poll() is None for p in self.procs.values())

    def signal_all(self, sig):
        for p in self.procs.values():
            p.send_signal(sig)

    def stop(self):
        codes = {}
        for p in self.procs.values():
            if p.poll() is None:
                p.send_signal(signal.SIGCONT)
                p.send_signal(signal.SIGTERM)
        for n, p in self.procs.items():
            try:
                codes[n] = p.wait(timeout=60)
            except subprocess.TimeoutExpired:
                p.kill()
                codes[n] = "killed"
        return codes

    def cleanup(self):
        subprocess.run(["psql", self.admin_url, "-qc",
                        f"DROP DATABASE IF EXISTS {self.name} WITH (FORCE)"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


class Rooms:
    """The pool rooms, the rooms visitors open one by one, and the highest seq any client has
    seen in each pool room."""

    def __init__(self, rng, fresh):
        def made():
            return str(uuid.UUID(int=rng.getrandbits(128), version=4))

        self.pool = [made() for _ in range(ROOM_POOL)]
        self.firehose = made()
        self.probe = made()
        self.fresh = [made() for _ in range(fresh)]
        self.lock = threading.Lock()
        self.heads = {}
        self.next_fresh = 0

    def open_fresh(self):
        """A room nobody has joined yet, or None once the run has used them all."""
        with self.lock:
            if self.next_fresh == len(self.fresh):
                return None
            self.next_fresh += 1
            return self.fresh[self.next_fresh - 1]

    def saw(self, room, seq):
        with self.lock:
            if seq > self.heads.get(room, 0):
                self.heads[room] = seq

    def head(self, room):
        with self.lock:
            return self.heads.get(room, 0)


class Actor(threading.Thread):
    """A thread holding one connection at a time, and what it hears on it."""

    def __init__(self, stack, counts, stop, rooms, seed):
        super().__init__(daemon=True)
        self.stack = stack
        self.counts = counts
        self.stop = stop
        self.rooms = rooms
        self.rng = random.Random(seed)
        self.ws = None
        self.joined = set()
        self.last_seq = {}
        self.hold_until = 0.0

    def connect(self, user, node=None, rcvbuf=None):
        node = node or self.rng.choice(NODES)
        self.ws = Ws.connect(self.stack.ports[node], self.stack.tokens[user], rcvbuf=rcvbuf)
        self.joined = set()
        self.counts.add("connections")

    def join_room(self, room, lossy=False, after=None):
        cmd = {"type": "join", "room": room}
        if lossy:
            cmd["delivery"] = "lossy"
        if after is not None:
            cmd["after"] = after
            self.counts.add("resumes")
        self.ws.send_json(cmd)

    def handle(self, text):
        m = json.loads(text)
        kind = m.get("type")
        room = m.get("room")
        if kind == "joined":
            self.joined.add(room)
        elif kind == "message":
            self.counts.add("messages_in")
            seq = m["seq"]
            if seq > self.last_seq.get(room, 0):
                self.last_seq[room] = seq
            self.rooms.saw(room, seq)
        elif kind == "sent":
            self.counts.add("acks")
        elif kind == "history":
            self.counts.add("history_pages")
        elif kind in ("watching", "presence"):
            self.counts.add("presence")
        elif kind == "error":
            reason = m.get("reason", "?")
            self.counts.add(f"error_{reason}")
            if reason == "rate_limited":
                self.hold_until = time.monotonic() + m.get("retry_after_ms", 500) / 1000
            if room and reason in ("not_member", "unavailable", "busy") and "id" not in m:
                self.joined.discard(room)

    def pump(self, seconds):
        end = time.monotonic() + seconds
        while not self.stop.is_set():
            left = end - time.monotonic()
            if left <= 0:
                return
            text = self.ws.recv(left)
            if text is None:
                return
            self.handle(text)

    def send_message(self, room, body=None, mid=None):
        mid = mid or uuid.uuid4().hex
        body = body if body is not None else os.urandom(self.rng.randint(16, 600))
        self.ws.send_json({"type": "send", "room": room, "id": mid, "body": b64(body)})
        self.counts.add("sends")
        return mid, body

    def hang_up(self):
        if self.ws:
            self.ws.close(clean=self.rng.random() < 0.5)
            self.ws = None


class Client(Actor):
    """One of --clients: a user who comes and goes, in the same rooms each time."""

    def __init__(self, stack, counts, stop, rooms, index, users, pause, scale):
        super().__init__(stack, counts, stop, rooms, index)
        self.scale = scale
        self.user = users[index]
        self.users = users
        self.pause = pause
        self.my_rooms = self.rng.sample(rooms.pool, ROOMS_PER_CLIENT)
        self.lossy_room = self.my_rooms[0] if index % 4 == 0 else None
        self.last_sent = None
        self.watching = []

    def act(self, name):
        if name == "watch" and not self.stack.features.presence:
            name = "ping"
        if name == "history" and not self.stack.features.history:
            name = "send"
        joined = sorted(self.joined)
        if name in ("send", "resend", "history") and not joined:
            return
        if name == "send":
            if time.monotonic() >= self.hold_until:
                room = self.rng.choice(joined)
                mid, body = self.send_message(room)
                self.last_sent = (room, mid, body)
        elif name == "resend":
            if self.last_sent and self.last_sent[0] in self.joined:
                room, mid, body = self.last_sent
                self.send_message(room, body, mid)
        elif name == "history":
            room = self.rng.choice(joined)
            cmd = {"type": "history", "room": room, "limit": self.rng.randint(10, 100)}
            top = self.last_seq.get(room, 0)
            if top > 1 and self.rng.random() < 0.5:
                cmd["before"] = self.rng.randint(1, top)
            self.ws.send_json(cmd)
        elif name == "watch":
            if len(self.watching) < 4:
                who = self.rng.choice([u for u in self.users if u != self.user])
                self.ws.send_json({"type": "watch", "user": who})
                self.watching.append(who)
            else:
                self.ws.send_json({"type": "unwatch", "user": self.watching.pop(0)})
        elif name == "ping":
            self.ws.send(0x9, b"soak")
        else:
            self.bad()

    def bad(self):
        room = str(uuid.uuid4())
        kind = self.rng.randrange(6)
        if kind == 0:
            self.ws.send(0x1, b"{not json")
        elif kind == 1:
            self.ws.send_json({"type": "shout", "room": room})
        elif kind == 2:
            self.ws.send_json({"type": "join", "room": room.upper()})
        elif kind == 3:
            self.ws.send_json({"type": "send", "room": room, "id": "no spaces!", "body": "AA"})
        elif kind == 4:
            self.ws.send_json({"type": "send", "room": room, "id": "x", "body": "***"})
        else:
            self.ws.send_json({"type": "send", "room": room, "id": "x", "body": "AA"})
        self.counts.add("bad_commands")

    def session(self):
        self.connect(self.user)
        self.watching = []
        for room in self.my_rooms:
            after = self.last_seq.get(room)
            self.join_room(room, lossy=room == self.lossy_room, after=after)
        end = time.monotonic() + self.rng.uniform(60, 300) * self.scale
        actions = [a for a, w in MIX for _ in range(w)]
        while not self.stop.is_set() and time.monotonic() < end:
            self.pump(self.rng.uniform(self.pause / 2, self.pause * 1.5))
            self.act(self.rng.choice(actions))

    def run(self):
        while not self.stop.is_set():
            try:
                self.session()
            except (OSError, Closed, Refused) as e:
                self.counts.add("transport_errors")
                self.counts.add(f"ended_{type(e).__name__}")
                self.stop.wait(1.0)
            except Exception as e:  # a bug in the soak must show, not end a client's load
                unexpected(self.counts, f"client {self.user}", e)
                self.stop.wait(1.0)
            finally:
                self.hang_up()


class Visitors(Actor):
    """A short connection every second, and now and then an upgrade that must be refused."""

    def __init__(self, stack, counts, stop, rooms):
        super().__init__(stack, counts, stop, rooms, 5000)
        self.in_flight = threading.Semaphore(VISITORS_IN_FLIGHT)
        self.n = 0

    def refused(self, kind):
        node = self.rng.choice(NODES)
        port = self.stack.ports[node]
        token = self.stack.tokens[f"v{self.n % VISITORS:02d}"]
        try:
            if kind == 0:
                Ws.connect(port).close(clean=False)
            elif kind == 1:
                Ws.connect(port, "forged.token.value").close(clean=False)
            elif kind == 2:
                Ws.connect(port, token, path="/nowhere").close(clean=False)
            else:
                Ws.connect(port, token, upgrade=False).close(clean=False)
            self.counts.add("upgrade_not_refused")
        except Refused as e:
            self.counts.add(f"upgrade_{e.status}")

    def visit(self, seed):
        actor = Actor(self.stack, self.counts, self.stop, self.rooms, seed)
        try:
            actor.connect(f"v{seed % VISITORS:02d}")
            for _ in range(actor.rng.randint(1, 2)):
                fresh = self.rooms.open_fresh() if actor.rng.random() < 0.2 else None
                if fresh:
                    self.counts.add("new_rooms")
                    actor.join_room(fresh)
                else:
                    room = actor.rng.choice(self.rooms.pool)
                    head = self.rooms.head(room)
                    actor.join_room(room, after=max(0, head - actor.rng.randint(0, 50)))
            actor.pump(actor.rng.uniform(0.5, 1.5))
            if actor.joined and actor.rng.random() < 0.5:
                actor.send_message(actor.rng.choice(sorted(actor.joined)))
            actor.pump(actor.rng.uniform(0.5, 2.0))
        except (OSError, Closed, Refused):
            self.counts.add("transport_errors")
        except Exception as e:  # as in Client.run
            unexpected(self.counts, "visitor", e)
        finally:
            actor.hang_up()
            self.in_flight.release()

    def run(self):
        while not self.stop.wait(1.0):
            self.n += 1
            if self.n % 10 == 0:
                self.guard(lambda: self.refused(self.n // 10 % 4))
                continue
            if not self.in_flight.acquire(blocking=False):
                self.counts.add("visitors_skipped")
                continue
            threading.Thread(target=self.visit, args=(self.n,), daemon=True).start()

    def guard(self, action):
        try:
            action()
        except (OSError, Closed):
            self.counts.add("transport_errors")
        except Exception as e:  # as in Client.run
            unexpected(self.counts, "refused upgrade", e)


class Burst(Actor):
    """Every 30 s, 16 sends at once from one user: ten fit the bucket, the rest are refused."""

    def __init__(self, stack, counts, stop, rooms):
        super().__init__(stack, counts, stop, rooms, 6000)

    def burst(self):
        if self.ws is None:
            self.connect("burst")
            self.join_room(self.rng.choice(self.rooms.pool))
            self.pump(2.0)
        if self.joined:
            room = next(iter(self.joined))
            for _ in range(16):
                self.send_message(room, os.urandom(32))
        self.pump(30.0)

    def run(self):
        while not self.stop.is_set():
            try:
                self.burst()
            except (OSError, Closed, Refused):
                self.counts.add("transport_errors")
                self.hang_up()
                self.stop.wait(5.0)
            except Exception as e:  # as in Client.run
                unexpected(self.counts, "burst", e)
                self.hang_up()
                self.stop.wait(5.0)
        self.hang_up()


class Slow(Actor):
    """Every 60 s, in turn: a consumer that stops reading, a durable one and a lossy one that
    read 8 KiB a second while a firehose sends them several times that."""

    def __init__(self, stack, counts, stop, rooms):
        super().__init__(stack, counts, stop, rooms, 7000)
        self.i = 0

    def hose(self, seed):
        """HOSE_MESSAGES of HOSE_BODY into the firehose room, two a second."""
        actor = Actor(self.stack, self.counts, self.stop, self.rooms, seed)
        try:
            actor.connect("hose")
            actor.join_room(self.rooms.firehose)
            actor.pump(1.0)
            for _ in range(HOSE_MESSAGES):
                if actor.joined and time.monotonic() >= actor.hold_until:
                    actor.send_message(self.rooms.firehose, os.urandom(HOSE_BODY))
                actor.pump(0.5)
        except (OSError, Closed, Refused):
            self.counts.add("transport_errors")
        except Exception as e:  # as in Client.run
            unexpected(self.counts, "firehose", e)
        finally:
            actor.hang_up()

    def one(self, kind, seed):
        actor = Actor(self.stack, self.counts, self.stop, self.rooms, seed)
        try:
            actor.connect(f"slow{seed % SLOW_MAX}", rcvbuf=SLOW_RCVBUF)
            actor.join_room(self.rooms.firehose, lossy=kind == "lossy")
            for room in actor.rng.sample(self.rooms.pool, 2):
                actor.join_room(room, lossy=kind == "lossy", after=0)
            if kind != "stalled":
                threading.Thread(target=self.hose, args=(seed,), daemon=True).start()
            actor.ws.sock.settimeout(0.2)
            for tick in range(150):
                if self.stop.wait(1.0):
                    break
                got = 0
                while kind != "stalled" and got < SLOW_READ:
                    try:
                        data = actor.ws.sock.recv(SLOW_READ - got)
                    except socket.timeout:
                        break
                    if not data:
                        raise Closed(0)
                    got += len(data)
                # A Ping now and then, or the node closes it as idle before it falls behind.
                if tick % 20 == 0:
                    actor.ws.send(0x9, b"slow")
            self.counts.add(f"slow_{kind}_held")
        except (OSError, Closed):
            self.counts.add(f"slow_{kind}_closed")
        except Exception as e:  # as in Client.run
            unexpected(self.counts, f"slow {kind}", e)
        finally:
            actor.hang_up()

    def run(self):
        while not self.stop.wait(60.0):
            self.i += 1
            kind = ["stalled", "durable", "lossy"][self.i % 3]
            threading.Thread(target=self.one, args=(kind, 7000 + self.i), daemon=True).start()


class Prefill(Actor):
    """The warm-up prefill: see PREFILL_FRAME and PREFILL_SENDERS."""

    def __init__(self, stack, counts, stop, rooms, until):
        super().__init__(stack, counts, stop, rooms, 8000)
        self.until = until

    def fill_pool(self, node):
        ws = Ws.connect(self.stack.ports[node], self.stack.tokens["fill"], mss=None)
        try:
            batch = Ws.frame_bytes(0x1, b"x" * PREFILL_FRAME) * PREFILL_BATCH
            for _ in range(PREFILL_FRAMES // PREFILL_BATCH):
                # recv() below leaves the socket's timeout at what it had left.
                ws.sock.settimeout(15)
                ws.sock.sendall(batch)
                self.counts.add("prefill_frames", PREFILL_BATCH)
                while ws.recv(0.05) is not None:
                    pass
        finally:
            ws.close(clean=True)

    def send(self, index):
        actor = Actor(self.stack, self.counts, self.stop, self.rooms, 8001 + index)
        room = self.rooms.pool[index % len(self.rooms.pool)]
        actor.connect(f"p{index // len(NODES):02d}", node=NODES[index % len(NODES)])
        try:
            actor.join_room(room)
            while time.time() < self.until and not self.stop.is_set():
                actor.pump(0.5)
                if room in actor.joined and time.monotonic() >= actor.hold_until:
                    actor.send_message(room)
                    self.counts.add("prefill_sends")
        finally:
            actor.hang_up()

    def guarded(self, action, *args):
        try:
            action(*args)
        except (OSError, Closed, Refused):
            self.counts.add("prefill_transport_errors")
        except Exception as e:  # as in Client.run
            unexpected(self.counts, "prefill", e)

    def run(self):
        parts = [threading.Thread(target=self.guarded, args=(self.fill_pool, n), daemon=True)
                 for n in NODES]
        parts += [threading.Thread(target=self.guarded, args=(self.send, i), daemon=True)
                  for i in range(PREFILL_SENDERS)]
        for t in parts:
            t.start()
        for t in parts:
            t.join()


class Signals(threading.Thread):
    """Owner changes by SIGSTOP and SIGCONT, one node in turn, and SIGHUP to all between."""

    def __init__(self, stack, counts, stop, every):
        super().__init__(daemon=True)
        self.stack = stack
        self.counts = counts
        self.stop = stop
        self.every = every

    def run(self):
        i = 0
        while not self.stop.wait(self.every / 2):
            if i % 2 == 0:
                node = NODES[(i // 2) % len(NODES)]
                p = self.stack.procs[node]
                p.send_signal(signal.SIGSTOP)
                self.stop.wait(OWNER_STOP_S)
                p.send_signal(signal.SIGCONT)
                self.counts.add("owner_changes")
                log(f"stopped {node} for {OWNER_STOP_S:.0f} s")
            else:
                self.stack.signal_all(signal.SIGHUP)
                self.counts.add("sighups")
            i += 1


def split(rows, warmup):
    return ([r for r in rows if r["elapsed_min"] < warmup],
            [r for r in rows if r["elapsed_min"] >= warmup])


def judge(rows, clients, warmup=WARMUP_MINUTES):
    """Returns (passed, report lines): ADR-0042's criterion, per node, on chat's units."""
    warm, window = split(rows, warmup)
    if len(window) < 10:
        return False, ["too few samples after the warm-up to judge"]
    hours = (window[-1]["elapsed_min"] - window[0]["elapsed_min"]) / 60
    report = [f"window: {len(window)} samples over {hours:.2f} h after a {warmup} min warm-up"]
    passed = True
    for node in NODES:
        c = column(node)
        rates = {unit: per_hour(window, f"{c}_{col}") for unit, col, _ in UNITS}
        rss = [(r["elapsed_min"] / 60, r[f"{c}_rss_kb"] * 1024) for r in window]
        peak = max(v for _, v in rss)
        slope, se = fit(rss)
        upper = slope + Z95 * se
        report.append(f"{node} rss: first {rss[0][1] / MIB:.1f} MiB, last {rss[-1][1] / MIB:.1f} "
                      f"MiB, peak {peak / MIB:.1f} MiB, slope {slope / 1e3:+.1f} KB/h, 95% upper "
                      f"end {upper / 1e3:+.1f} KB/h; per hour " +
                      ", ".join(f"{rates[u]:.0f} {col}" for u, col, _ in UNITS))
        for unit, _, production in UNITS:
            bound = (MEMORY_LIMIT - peak) / (production * MONTH_SECONDS)
            if rates[unit] <= 0:
                report.append(f"  per {unit}: none in the window: NOT JUDGED")
                passed = False
                continue
            leak = upper / rates[unit]
            ok = leak < bound
            passed = passed and ok
            report.append(f"  per {unit}: at most {leak:.4f} B, bound {bound:.4f} B: "
                          f"{'flat' if ok else 'NOT SHOWN FLAT'}")
        fds = [(r["elapsed_min"] / 60, r[f"{c}_fds"]) for r in window]
        fd_slope, fd_se = fit(fds)
        fd_rise = (fd_slope + Z95 * fd_se) * hours
        warm_fds = [r[f"{c}_fds"] for r in warm] or [0]
        bound = max(fd_bound(clients), max(warm_fds) - min(warm_fds))
        ok = fd_rise < bound
        passed = passed and ok
        report.append(f"{node} fds: first {fds[0][1]}, last {fds[-1][1]}, min "
                      f"{min(v for _, v in fds)}, max {max(v for _, v in fds)}, rise over the "
                      f"window at most {fd_rise:+.2f}, bound {bound}: "
                      f"{'flat' if ok else 'NOT FLAT'}")
    return passed, report


def coverage(rows, totals, features):
    """Each path of the mix, and whether the run exercised it: (all ran, report lines)."""
    last = rows[-1] if rows else {}

    def metric(name):
        return sum(last.get(f"{column(n)}_{name}", 0) for n in NODES)

    checks = [("messages delivered on every node",
               all(last.get(f"{column(n)}_deliveries", 0) > 0 for n in NODES)),
              ("sends forwarded to another node's room", metric("forwards") > 0),
              ("rooms taken over from a stopped owner", metric("reassignments") > 0),
              ("stale owner writes fenced out", metric("fenced") > 0),
              ("sends rate limited", metric("rate_limited") > 0),
              ("resends deduplicated", metric("deduplicated") > 0),
              ("resumes replayed kept messages", metric("replayed") > 0),
              ("lossy clients skipped", metric("lossy_drops") > 0),
              ("slow consumers closed", metric("slow_consumers") > 0),
              ("upgrades refused (401, 404, 426)",
               all(totals.get(f"upgrade_{s}", 0) > 0 for s in (401, 404, 426))),
              ("bad commands answered", all(totals.get(f"error_{r}", 0) > 0 for r in (
                  "not_json", "malformed", "bad_room", "bad_id", "bad_body", "not_joined"))),
              ("SIGHUPs taken", totals.get("sighups", 0) > 0),
              ("no exception in the soak itself", totals.get("soak_exceptions", 0) == 0),
              ("warm-up prefill ran",
               totals.get("prefill_frames", 0) == len(NODES) * PREFILL_FRAMES and
               totals.get("prefill_sends", 0) > 0)]
    if features.history:
        checks.append(("history pages", totals.get("history_pages", 0) > 0))
    if features.presence:
        checks.append(("presence answers and events", totals.get("presence", 0) > 0))
    report = [f"  {name}: {'yes' if ok else 'NO'}" for name, ok in checks]
    return all(ok for _, ok in checks), report


def load_samples(path):
    rows = []
    with open(path) as f:
        for r in csv.DictReader(f):
            rows.append({k: (float(v) if k == "elapsed_min" else (int(v) if v.isdigit() else v))
                         for k, v in r.items()})
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--build", default=str(ROOT / "build" / "ci"))
    parser.add_argument("--hours", type=float, default=6.0)
    parser.add_argument("--out")
    parser.add_argument("--port", type=int, default=19101,
                        help="first of three client ports; node ports are 100 above")
    parser.add_argument("--clients", type=int, default=64)
    parser.add_argument("--pause", type=float, default=0.8,
                        help="mean seconds between one client's actions")
    parser.add_argument("--interval", type=float, default=60.0, help="seconds between samples")
    parser.add_argument("--self-test", action="store_true",
                        help="12 minutes, 15 s samples, owner changes every 150 s")
    parser.add_argument("--rejudge", metavar="SAMPLES_CSV")
    args = parser.parse_args()
    if args.rejudge:
        passed, report = judge(load_samples(args.rejudge), args.clients)
        print("\n".join(report + [f"verdict: {'PASS' if passed else 'FAIL'}"]))
        return 0 if passed else 1
    if not args.out:
        parser.error("--out is required for a run")
    warmup, scale, every = WARMUP_MINUTES, 1.0, 600.0
    if args.self_test:
        args.hours, args.interval = 0.2, 15.0
        warmup, scale, every = 3, 0.3, 150.0

    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    stack = Stack(args, out)
    users = ([f"c{i:02d}" for i in range(args.clients)] + [f"v{i:02d}" for i in range(VISITORS)] +
             [f"slow{i}" for i in range(SLOW_MAX)] + ["burst", "hose", "probe", "fill"] +
             [f"p{i:02d}" for i in range(PREFILL_SENDERS // len(NODES))])
    # Visitors open one room in about every five seconds: a fifth of one visit a second.
    rooms = Rooms(random.Random(42), int(args.hours * 3600 / 5 * 1.5) + 100)
    stack.copy_binaries()
    stack.prepare(users)
    stack.grant(rooms.pool + [rooms.firehose, rooms.probe], users)
    stack.grant(rooms.fresh, [u for u in users if u.startswith("v")])
    # Whatever start() launched before it failed is stopped: the soak may run as root, where
    # nothing else would stop it.
    try:
        stack.start()
    except BaseException:
        stack.stop()
        raise
    stack.detect("probe", "hose", rooms.probe)
    pids = stack.pids()
    (out / "pids.json").write_text(json.dumps({**pids, "soak": os.getpid()}))
    absent = stack.features.absent()
    log(f"started: {pids}, database {stack.name}; not on this server: "
        f"{', '.join(absent) if absent else 'nothing'}")

    stop = threading.Event()
    counts = Counts()
    client_users = users[:args.clients]
    threads = [Client(stack, counts, stop, rooms, i, client_users, args.pause, scale)
               for i in range(args.clients)]
    threads += [Visitors(stack, counts, stop, rooms), Burst(stack, counts, stop, rooms),
                Slow(stack, counts, stop, rooms), Signals(stack, counts, stop, every),
                Prefill(stack, counts, stop, rooms,
                        time.time() + (warmup - PREFILL_MARGIN_MINUTES) * 60)]
    for t in threads:
        t.start()

    fields = ["time", "elapsed_min"]
    for node in NODES:
        fields += [f"{column(node)}_{k}" for k in ["rss_kb", "fds", *METRICS]]
    fields += DRIVER
    rows = []
    last_metrics = {n: {} for n in NODES}
    started = time.time()
    end = started + args.hours * 3600
    failure = None
    with open(out / "samples.csv", "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        next_sample = started
        while time.time() < end:
            if not stack.alive():
                failure = "a node exited during the run"
                break
            if time.time() >= next_sample:
                row = {"time": time.strftime("%Y-%m-%dT%H:%M:%S"),
                       "elapsed_min": round((time.time() - started) / 60, 2)}
                for node in NODES:
                    c = column(node)
                    row[f"{c}_rss_kb"], row[f"{c}_fds"] = proc_sample(pids[node])
                    # A stopped node cannot answer; its counters only ever rise, so the last
                    # ones it gave stand in.
                    m = stack.metrics(node) or last_metrics[node]
                    last_metrics[node] = m
                    for k, name in METRICS.items():
                        row[f"{c}_{k}"] = m.get(name, 0)
                snapshot = counts.snapshot()
                row.update({k: snapshot.get(k, 0) for k in DRIVER})
                writer.writerow(row)
                f.flush()
                rows.append(row)
                next_sample += args.interval
            time.sleep(min(5.0, max(0.0, next_sample - time.time())))

    stop.set()
    for t in threads:
        t.join(timeout=30)
    codes = stack.stop()
    stack.cleanup()
    totals = counts.snapshot()
    flat, report = judge(rows, args.clients, warmup)
    covered, covered_report = coverage(rows, totals, stack.features)
    summary = [f"chat soak: {args.hours} h requested{' (self-test)' if args.self_test else ''}, "
               f"{len(rows)} samples, {args.clients} clients",
               f"not on this server, left out: {', '.join(absent) if absent else 'nothing'}",
               f"exit codes after SIGTERM: {codes}",
               f"totals: {json.dumps(totals, sort_keys=True)}", "paths exercised:",
               *covered_report, *report]
    if failure:
        summary.append(f"FAILED: {failure}")
    exited = all(v == 0 for v in codes.values())
    if args.self_test:
        summary.append("flatness, not a verdict in a self-test: "
                       f"{'flat' if flat else 'not shown'}")
        passed = covered and exited and not failure
    else:
        passed = flat and covered and exited and not failure
    summary.append(f"verdict: {'PASS' if passed else 'FAIL'}")
    (out / "summary.txt").write_text("\n".join(summary) + "\n")
    print("\n".join(summary), flush=True)
    if failure:
        return 2
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
