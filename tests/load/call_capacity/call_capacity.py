#!/usr/bin/env python3
"""Call capacity against its derivation (brief 8.1, ADR-0012): measured on the SFU itself.

The derivation sizes a realtime node by bandwidth: 540 Mbit/s usable, and a 1:1 call costs the
SFU 2 x 700 kbps out (each participant receives the other's stream), so 540 / 1.4 = 385 calls.
It rests on one claim about the SFU: in a 1:1 call it sends each participant what the other
sent it, no more. This checks that claim on the pinned LiveKit, configured as the overlay
configures it (one UDP port, single-layer publishing as 1:1 calls use), and measures the CPU a
call costs so the CPU ceiling can be set beside the bandwidth one:

    tests/load/call_capacity/call_capacity.py [--calls 4] [--participants 2] [--seconds 60]
                                              [--out DIR]

--participants n makes each call a group call of n (M29, ADR-0095): every participant publishes
and receives the n - 1 others, so the derivation is n x (n - 1) x 700 kbps out per call, n x
700 kbps in, and out over in is n - 1. The layer is stated, not left to chance: one layer,
simulcast off, so every subscriber receives the top (and only) one, which is the most a group
call can cost the SFU. tools/group_call_capacity_test.sh runs it for rooms of four.

--calls rooms of two participants each publish a 640x360 video track capped at 700 kbps and
subscribe to each other, every participant through the LiveKit Python SDK. LiveKit runs in a
Docker container of its own on the default bridge, so the container's interface counters are
the SFU's traffic and nothing else's, and the server process's CPU time is the SFU's. After a warm-up,
bytes in, bytes out and CPU time are read at the start and end of the measured window.

The run fails if a call's egress on the wire, packet headers included, is more than 10% away
from the derived 1.4 Mbit/s, or if the SFU sends more than 10% more or less than it receives,
or if the load is void: a participant never received its peer's track, or the publishers
delivered under half their 700 kbps (the encoder was starved, so nothing would be said about
calls at the planned rate). The report gives the bandwidth ceiling at the measured egress per
call, 540 / egress, next to the derivation's 385, and the CPU ceiling of the overlay's 2-core
limit at the measured CPU per call; whichever is lower is the node's call capacity (ADR-0012).

Everything runs on this machine: a local container and local clients. Needs Docker and the
packages in requirements.txt.
"""
import argparse
import asyncio
import base64
import hashlib
import hmac
import json
import os
import pathlib
import subprocess
import sys
import time
import urllib.request

from livekit import rtc

# docker.io/livekit/livekit-server:v1.13.7, the digest deploy/local/images.sh pins.
IMAGE = ("docker.io/livekit/livekit-server:v1.13.7@sha256:"
         "6fd3b7088874c4d119160dd688798dfec852bc014786d392caad15f6f63912a3")
API_KEY = "callcap"
# Local to this run's container, which listens only on the Docker bridge.
API_SECRET = "callcap-local-secret-at-least-32-characters"
# Brief 8.1: 600 Mbit/s less 10%, and 700 kbps each way per participant.
USABLE_MBPS = 540.0
PUBLISH_BPS = 700_000
TOLERANCE = 0.10
# The base's cpu limit for the SFU (deploy/kubernetes/base/livekit/deployment.yaml).
SFU_CORES = 2.0
WIDTH, HEIGHT, FPS = 640, 360, 30
# Distinct noise frames, cycled: an encoder cannot compress noise, so every track sends at its
# cap, which is what the derivation assumes of a call.
NOISE_FRAMES = 12
WARMUP_S = 15


def token(identity, room):
    def b64(raw):
        return base64.urlsafe_b64encode(raw).rstrip(b"=")
    now = int(time.time())
    claims = {"iss": API_KEY, "sub": identity, "nbf": now - 10, "exp": now + 3600,
              "video": {"room": room, "roomJoin": True, "canPublish": True,
                        "canSubscribe": True}}
    head = b64(json.dumps({"alg": "HS256", "typ": "JWT"}).encode()) + b"." + \
        b64(json.dumps(claims).encode())
    signature = hmac.new(API_SECRET.encode(), head, hashlib.sha256).digest()
    return (head + b"." + b64(signature)).decode()


def docker(*args, capture=True):
    try:
        return subprocess.run(["docker", *args], check=True, text=True,
                              capture_output=capture).stdout.strip()
    except subprocess.CalledProcessError as e:
        # Docker's own reason (a registry refusal, a name in use) is on its stderr.
        if e.stderr:
            sys.stderr.write(e.stderr)
        raise


def pull(attempts=5):
    """The image, before anything is measured: a registry that refuses a pull now and then
    (a rate limit, a reset) is retried, and one that keeps refusing fails here, not mid-test."""
    for attempt in range(attempts):
        try:
            docker("pull", "-q", IMAGE)
            return
        except subprocess.CalledProcessError:
            if attempt + 1 == attempts:
                raise
            time.sleep(2 ** attempt)


class Sfu:
    """The LiveKit container, and its traffic and CPU as the kernel counts them."""

    def __init__(self, name):
        config = {
            "port": 7880,
            "keys": {API_KEY: API_SECRET},
            "rtc": {"udp_port": 7882, "tcp_port": 0, "use_external_ip": False},
            "room": {"auto_create": True},
            "logging": {"level": "warn"},
        }
        docker("run", "-d", "--rm", "--name", name, IMAGE, "--config-body", json.dumps(config))
        self.pid = docker("inspect", "-f", "{{.State.Pid}}", name)
        # Docker 29 no longer fills the top-level IPAddress; the bridge network's entry has it.
        self.ip = docker("inspect", "-f",
                         "{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}", name)

    def url(self):
        return f"ws://{self.ip}:7880"

    def wait_ready(self, limit_s=60):
        deadline = time.monotonic() + limit_s
        while time.monotonic() < deadline:
            try:
                with urllib.request.urlopen(f"http://{self.ip}:7880/", timeout=2) as r:
                    if r.status == 200:
                        return
            except OSError:
                pass
            time.sleep(0.5)
        raise RuntimeError("LiveKit never answered on " + self.ip)

    def counters(self):
        # /proc/<pid>/net/dev is the netns of that process: the container's eth0 alone.
        for line in pathlib.Path(f"/proc/{self.pid}/net/dev").read_text().splitlines():
            name, _, rest = line.partition(":")
            if name.strip() == "eth0":
                fields = rest.split()
                rx, tx = int(fields[0]), int(fields[8])
                break
        else:
            raise RuntimeError("the SFU has no eth0")
        # livekit-server is the container's only process; utime and stime follow the command,
        # whose parentheses may hold spaces, so fields are counted from the closing one.
        stat = pathlib.Path(f"/proc/{self.pid}/stat").read_text()
        fields = stat[stat.rindex(")") + 2:].split()
        ticks = int(fields[11]) + int(fields[12])
        usec = ticks * 1_000_000 // os.sysconf("SC_CLK_TCK")
        return {"t": time.monotonic(), "rx": rx, "tx": tx, "cpu_usec": usec}


class Participant:
    def __init__(self, room_name, identity, frames):
        self.room_name = room_name
        self.identity = identity
        self.frames = frames
        self.room = rtc.Room()
        self.source = rtc.VideoSource(WIDTH, HEIGHT)
        self.subscribed = asyncio.Event()
        self.tracks = 0
        self.room.on("track_subscribed", self._subscribed)

    def _subscribed(self, *_):
        self.tracks += 1
        self.subscribed.set()

    async def all_subscribed(self, count):
        while self.tracks < count:
            await asyncio.sleep(0.1)

    async def join(self, url):
        await self.room.connect(url, token(self.identity, self.room_name),
                                rtc.RoomOptions(auto_subscribe=True))
        track = rtc.LocalVideoTrack.create_video_track("camera", self.source)
        options = rtc.TrackPublishOptions(
            source=rtc.TrackSource.SOURCE_CAMERA, simulcast=False,
            video_codec=rtc.VideoCodec.VP8,
            video_encoding=rtc.VideoEncoding(max_bitrate=PUBLISH_BPS, max_framerate=FPS))
        await self.room.local_participant.publish_track(track, options)

    async def send(self, stop):
        n = 0
        start = time.monotonic()
        while not stop.is_set():
            self.source.capture_frame(self.frames[n % len(self.frames)])
            n += 1
            await asyncio.sleep(max(0.0, start + n / FPS - time.monotonic()))

    async def leave(self):
        await self.room.disconnect()


def i420_noise():
    size = WIDTH * HEIGHT * 3 // 2
    return [rtc.VideoFrame(WIDTH, HEIGHT, rtc.VideoBufferType.I420, os.urandom(size))
            for _ in range(NOISE_FRAMES)]


async def drive(sfu, calls, seconds, participants=2):
    frames = i420_noise()
    people = [Participant(f"call-{c}", f"call-{c}-{side}", frames)
              for c in range(calls) for side in range(participants)]
    for p in people:
        await p.join(sfu.url())
    stop = asyncio.Event()
    senders = [asyncio.create_task(p.send(stop)) for p in people]
    try:
        await asyncio.wait_for(asyncio.gather(*(p.subscribed.wait() for p in people)), 30)
        # Every one of the n - 1 others' tracks, not just the first, before anything is measured.
        await asyncio.wait_for(asyncio.gather(*(p.all_subscribed(participants - 1)
                                                for p in people)), 30)
    except asyncio.TimeoutError:
        missing = [p.identity for p in people if not p.subscribed.is_set()]
        raise RuntimeError(f"never received the peer's track: {missing}") from None
    await asyncio.sleep(WARMUP_S)
    first = sfu.counters()
    await asyncio.sleep(seconds)
    last = sfu.counters()
    stop.set()
    await asyncio.gather(*senders)
    for p in people:
        await p.leave()
    return first, last


def judge(calls, first, last, participants=2):
    elapsed = last["t"] - first["t"]
    in_mbps = (last["rx"] - first["rx"]) * 8 / elapsed / 1e6
    out_mbps = (last["tx"] - first["tx"]) * 8 / elapsed / 1e6
    cores = (last["cpu_usec"] - first["cpu_usec"]) / 1e6 / elapsed
    ratio = out_mbps / in_mbps if in_mbps > 0 else float("inf")
    n = participants
    per_publisher_kbps = in_mbps * 1000 / (n * calls)
    # Each of the n publishers' streams goes to the n - 1 others.
    derived_call_mbps = n * (n - 1) * PUBLISH_BPS / 1e6
    report = {
        "calls": calls,
        "participants_per_call": n,
        "assumed_layer": "one layer, simulcast off: every subscriber receives the top one",
        "derived_out_mbps_per_call": round(derived_call_mbps, 3),
        "seconds": round(elapsed, 1),
        "sfu_in_mbps": round(in_mbps, 3),
        "sfu_out_mbps": round(out_mbps, 3),
        "out_over_in": round(ratio, 4),
        "per_publisher_kbps": round(per_publisher_kbps, 1),
        "out_mbps_per_call": round(out_mbps / calls, 4),
        "sfu_cores": round(cores, 4),
        "sfu_cores_per_call": round(cores / calls, 5),
        "derived_calls": int(USABLE_MBPS / derived_call_mbps),
        "bandwidth_ceiling_calls": int(USABLE_MBPS / (out_mbps / calls)),
        "cpu_ceiling_calls": int(SFU_CORES / (cores / calls)) if cores > 0 else None,
    }
    failures = []
    if per_publisher_kbps < PUBLISH_BPS / 1000 / 2:
        failures.append(f"publishers sent {per_publisher_kbps:.0f} kbps each, under half the "
                        f"{PUBLISH_BPS // 1000} kbps the check needs")
    per_call = out_mbps / calls
    if abs(per_call / derived_call_mbps - 1) > TOLERANCE:
        failures.append(f"a call cost the SFU {per_call:.3f} Mbit/s out against the derived "
                        f"{derived_call_mbps:.3f}; the derivation holds within {TOLERANCE:.0%}")
    if abs(ratio / (n - 1) - 1) > TOLERANCE:
        failures.append(f"the SFU sent {ratio:.3f}x what it received, against {n - 1}x; the "
                        f"derivation holds within {TOLERANCE:.0%}")
    report["failures"] = failures
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--calls", type=int, default=4)
    parser.add_argument("--participants", type=int, default=2)
    parser.add_argument("--seconds", type=int, default=60)
    parser.add_argument("--out", type=pathlib.Path)
    args = parser.parse_args()

    name = f"ulw-callcap-{os.getpid()}"
    try:
        pull()
        sfu = Sfu(name)
        sfu.wait_ready()
        if args.participants < 2:
            parser.error("--participants: a call has two at least")
        first, last = asyncio.run(drive(sfu, args.calls, args.seconds, args.participants))
    finally:
        subprocess.run(["docker", "rm", "-f", name], capture_output=True)
    report = judge(args.calls, first, last, args.participants)
    text = json.dumps(report, indent=2)
    print(text)
    if args.out:
        args.out.mkdir(parents=True, exist_ok=True)
        (args.out / "report.json").write_text(text + "\n")
    for failure in report["failures"]:
        print("FAIL:", failure, file=sys.stderr)
    return 1 if report["failures"] else 0


if __name__ == "__main__":
    sys.exit(main())
