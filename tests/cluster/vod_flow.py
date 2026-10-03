#!/usr/bin/env python3
"""End-to-end checks of the VOD plane in the sandbox cluster (deploy/local/e2e-up.sh).

Every request goes through Envoy's HTTPRoute at 127.0.0.1:18080 with a token minted by the
mock auth-service, as a browser's would through askedin-gateway:

  auth        each signing algorithm, the cookie, key rotation, refusals, and the
              askedin-gateway stand-in replacing forged x-user-* headers with the token's
  upload      a clip uploaded chunk by chunk, transcoded by the worker, reaching ready
  playback    the master and every media playlist through the route, and every init and media
              segment from the object store at the presigned URLs they carry, none from the
              gateway
  pod-kill    the gateway pod serving an upload deleted mid-chunk; the chunk is cut off, the
              client asks HEAD for the durable offset, finishes against the pods left, and the
              video reaches ready
  netpol      a pod beside the gateway cannot open a connection to it (x-user-id forgery has
              nothing to reach), while it can reach the auth-service; the worker reaches the
              store but neither the gateway nor the auth-service
  chat        the chat Deployment's three nodes through the route: an upgrade on /rt without a
              token is refused, a stream's live chat opened as the RUNBOOK opens it is joined
              by several sockets, and a message sent on one reaches every one of them, wherever
              Envoy put them; a pod beside chat cannot reach its node-channel port

    tests/cluster/vod_flow.py [--allow-skip] [SCENARIO...]     all of them when none is named

A skipped scenario fails the run unless --allow-skip is given. Needs ffmpeg on PATH for the test
clips.

kubectl only ever runs against the sandbox: deploy/local/.state/kubeconfig with context
kind-ulw-e2e, whose API server must be on 127.0.0.1, checked before anything runs. The
caller's KUBECONFIG is ignored.

Against a real deployment (deploy/askedin/RUNBOOK.md), ULW_E2E_URL names the Gateway
(https://host) and ULW_E2E_TOKEN a token its auth-service issued. Only upload and playback may
run there, only when named, and nothing calls kubectl: the scenarios that delete pods, start
pods or drive the mock auth-service are refused.
"""
import base64
import http.client
import json
import os
import pathlib
import re
import socket
import subprocess
import sys
import tempfile
import time
import urllib.parse
import uuid

import yaml

ROOT = pathlib.Path(__file__).resolve().parents[2]
KUBECTL = str(ROOT / "deploy/local/.tools/kubectl")
SANDBOX_KUBECONFIG = ROOT / "deploy/local/.state/kubeconfig"
SANDBOX_CONTEXT = "kind-ulw-e2e"
SANDBOX_URL = "http://127.0.0.1:18080"
# e2e-up.sh publishes MinIO here; presigned URLs name it minio:9000, as the pods do.
SANDBOX_STORE = {"minio:9000": ("127.0.0.1", 19000)}
REAL_TARGET_SCENARIOS = {"upload", "playback"}
NAMESPACE = "apps-stage"
# The sandbox's Postgres container (e2e-up.sh), where the chat scenario opens a stream's chat.
SANDBOX_PG = "ulw-e2e-pg"
# A clip at 1280x720 transcodes to two rungs (720p, 360p) in well under a minute on the
# sandbox's half-core worker; the clock allows for a slow machine.
READY_TIMEOUT_S = 600

# Set by main(): where requests go, the token to use instead of minting, and whether kubectl
# may run at all.
BASE = urllib.parse.urlsplit(SANDBOX_URL)
TOKEN = None
KUBECTL_ALLOWED = False


class Failure(Exception):
    pass


class Skipped(Exception):
    pass


class Refused(Exception):
    pass


def require_sandbox(kubeconfig):
    """Refuses unless `kubeconfig` exists and its SANDBOX_CONTEXT names a kind cluster on this
    machine's loopback, where kind binds every API server it creates. Reads the file itself:
    nothing here may run kubectl before the check has passed."""
    if not kubeconfig.is_file():
        raise Refused(f"{kubeconfig} is missing; run make e2e-up first")
    config = yaml.safe_load(kubeconfig.read_text(encoding="utf-8")) or {}
    context = next((c["context"] for c in config.get("contexts") or []
                    if c.get("name") == SANDBOX_CONTEXT), None)
    if context is None:
        raise Refused(f"{kubeconfig} has no context {SANDBOX_CONTEXT}")
    server = next((c["cluster"].get("server", "") for c in config.get("clusters") or []
                   if c.get("name") == context.get("cluster")), "")
    url = urllib.parse.urlsplit(server)
    if url.scheme != "https" or url.hostname != "127.0.0.1":
        raise Refused(f"context {SANDBOX_CONTEXT} points at {server!r}, not a kind cluster on "
                      "this machine")


def plan(names, env):
    """The scenarios to run and the target, or Refused. A real target (ULW_E2E_URL) runs only
    the scenarios in REAL_TARGET_SCENARIOS, named explicitly; the sandbox runs any."""
    unknown = [n for n in names if n not in SCENARIOS]
    if unknown:
        raise Refused(f"unknown scenario {unknown}; choose from {list(SCENARIOS)}")
    url = env.get("ULW_E2E_URL")
    if url is None:
        return list(names or SCENARIOS), SANDBOX_URL, None
    if not names:
        raise Refused("ULW_E2E_URL is set: name the scenarios to run against it "
                      f"({', '.join(sorted(REAL_TARGET_SCENARIOS))})")
    forbidden = [n for n in names if n not in REAL_TARGET_SCENARIOS]
    if forbidden:
        raise Refused(f"{forbidden} never run against ULW_E2E_URL: they delete or start pods "
                      "or drive the mock auth-service")
    token = env.get("ULW_E2E_TOKEN")
    if not token:
        raise Refused("ULW_E2E_URL needs ULW_E2E_TOKEN, a token its auth-service issued")
    return list(names), url, token


def check(condition, message):
    if not condition:
        raise Failure(message)


def connect(timeout):
    kind = http.client.HTTPSConnection if BASE.scheme == "https" else http.client.HTTPConnection
    return kind(BASE.hostname, BASE.port, timeout=timeout)


def request(method, path, token=None, body=None, headers=None, cookie=None):
    conn = connect(60)
    all_headers = dict(headers or {})
    if token:
        all_headers["Authorization"] = f"Bearer {token}"
    if cookie:
        all_headers["Cookie"] = cookie
    if isinstance(body, (dict, list)):
        body = json.dumps(body).encode()
        all_headers["Content-Type"] = "application/json"
    conn.request(method, path, body=body, headers=all_headers)
    response = conn.getresponse()
    data = response.read()
    conn.close()
    # Envoy lowercases header names on the way out.
    return response.status, {k.lower(): v for k, v in response.getheaders()}, data


def mint(subject, alg="ES256"):
    if TOKEN:
        return TOKEN
    status, headers, data = request("POST", f"/mock-auth/token?sub={subject}&alg={alg}"
                                    f"&email={subject}@ulw-sandbox.test")
    check(status == 200, f"mint {alg}: {status} {data!r}")
    check("auth_token_stage=" in headers.get("set-cookie", ""), "mint set no stage cookie")
    return json.loads(data)["token"]


def kubectl(*args, check_rc=True):
    if not KUBECTL_ALLOWED:
        raise Refused("kubectl is only run against the sandbox")
    result = subprocess.run([KUBECTL, "--kubeconfig", str(SANDBOX_KUBECONFIG),
                             "--context", SANDBOX_CONTEXT, *args],
                            capture_output=True, text=True)
    if check_rc and result.returncode != 0:
        raise Failure(f"kubectl {' '.join(args)}: {result.stderr.strip()}")
    return result


def sandbox_sql(sql):
    """Runs `sql` in the sandbox's Postgres as its superuser, as the RUNBOOK's statements run on
    Askedin's; only ever against the sandbox, like kubectl."""
    if not KUBECTL_ALLOWED:
        raise Refused("SQL is only run against the sandbox")
    result = subprocess.run(["docker", "exec", SANDBOX_PG, "psql", "-U", "postgres", "-qAt",
                             "-v", "ON_ERROR_STOP=1", "-c", sql], capture_output=True, text=True)
    if result.returncode != 0:
        raise Failure(f"psql: {result.stderr.strip()}")
    return result.stdout.strip()


def unknown_video_id():
    # Shaped like a real UUIDv7 id (version 7, variant 10) but never issued.
    raw = bytearray(uuid.uuid4().bytes)
    raw[6] = (raw[6] & 0x0F) | 0x70
    raw[8] = (raw[8] & 0x3F) | 0x80
    return str(uuid.UUID(bytes=bytes(raw)))


def make_clip(path, seconds, bitrate):
    # Temporal noise keeps x264 from undershooting the bitrate on the synthetic pattern, so
    # the clip is as large as the pod-kill scenario needs.
    subprocess.run(
        ["ffmpeg", "-nostdin", "-loglevel", "error", "-y",
         "-f", "lavfi", "-i", f"testsrc2=size=1280x720:rate=30:duration={seconds}",
         "-f", "lavfi", "-i", f"sine=frequency=440:duration={seconds}",
         "-vf", "noise=alls=30:allf=t", "-c:v", "libx264", "-preset", "veryfast",
         "-b:v", bitrate, "-maxrate", bitrate, "-bufsize", bitrate,
         "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest", str(path)],
        check=True)
    return path.read_bytes()


def create_upload(token, name, size):
    status, _, data = request("POST", "/api/v1/uploads", token,
                              {"filename": name, "size_bytes": size, "content_type": "video/mp4"})
    check(status == 201, f"create upload: {status} {data!r}")
    return json.loads(data)


def offset_of(token, upload_id):
    # A client resuming after a failure keeps asking until a gateway answers: with every pod
    # replaced at once, Envoy has no endpoint (503) until the first new one is ready.
    deadline = time.monotonic() + 120
    while True:
        try:
            status, headers, _ = request("HEAD", f"/api/v1/uploads/{upload_id}", token)
        except OSError as e:
            status, headers = type(e).__name__, {}
        if status in (200, 204):
            return int(headers["upload-offset"])
        check(time.monotonic() < deadline, f"HEAD upload: {status}")
        time.sleep(1)


def patch(token, upload_id, offset, chunk):
    status, headers, data = request(
        "PATCH", f"/api/v1/uploads/{upload_id}", token, chunk,
        {"Upload-Offset": str(offset), "Content-Type": "application/offset+octet-stream"})
    check(status == 204, f"PATCH at {offset}: {status} {data!r}")
    return int(headers["upload-offset"])


def send_from(token, upload_id, data, offset, chunk_size):
    while offset < len(data):
        offset = patch(token, upload_id, offset, data[offset:offset + chunk_size])
    return offset


def commit_and_wait_ready(token, upload):
    status, _, data = request("POST", f"/api/v1/uploads/{upload['upload_id']}/commit", token)
    check(status == 200, f"commit: {status} {data!r}")
    deadline = time.monotonic() + READY_TIMEOUT_S
    video = upload["video_id"]
    while True:
        status, _, data = request("GET", f"/api/v1/videos/{video}", token)
        check(status == 200, f"GET video: {status} {data!r}")
        state = json.loads(data)
        if state["state"] == "ready":
            check(state["duration_ms"] and state["duration_ms"] > 0, f"ready without duration: {state}")
            return state
        check(state["state"] != "failed", f"video failed: {state}")
        check(time.monotonic() < deadline, f"not ready after {READY_TIMEOUT_S} s: {state}")
        time.sleep(2)


def check_askedin_gateway_stand_in(subject, token):
    """The askedin-gateway stand-in (deploy/local/cluster/askedin-identity.yaml) drops
    x-user-* headers a client sends and injects the token's own; /askedin-service/whoami
    echoes what arrived behind it."""
    forged = {"x-user-id": "someone-else", "x-user-email": "someone-else@ulw-sandbox.test"}
    status, _, data = request("GET", "/askedin-service/whoami", token, headers=forged)
    check(status == 200, f"whoami with a token: {status} {data!r}")
    seen = json.loads(data)
    check(seen == {"x-user-id": [subject], "x-user-email": [f"{subject}@ulw-sandbox.test"]},
          f"behind the stand-in, expected only the token's identity, got {seen}")
    status, _, data = request("GET", "/askedin-service/whoami", headers=forged)
    check(status == 401, f"whoami with forged headers and no token: {status} {data!r}")


def scenario_auth():
    subject = f"auth-{uuid.uuid4().hex[:8]}"
    missing = f"/api/v1/videos/{unknown_video_id()}"
    for alg in ("RS256", "PS256", "ES256", "EdDSA"):
        status, _, _ = request("GET", missing, mint(subject, alg))
        check(status == 404, f"{alg} token: expected 404 for an unknown video, got {status}")
    token = mint(subject)
    status, _, _ = request("GET", missing, cookie=f"auth_token_stage={token}")
    check(status == 404, f"cookie token: expected 404, got {status}")
    status, _, _ = request("GET", missing)
    check(status == 401, f"no token: expected 401, got {status}")
    status, _, _ = request("GET", missing, headers={"x-user-id": subject})
    check(status == 401, f"forged x-user-id without a token: expected 401, got {status}")
    signed, signature = token.rsplit(".", 1)
    tampered = f"{signed}.{'B' if signature[0] == 'A' else 'A'}{signature[1:]}"
    status, _, _ = request("GET", missing, tampered)
    check(status == 401, f"tampered signature: expected 401, got {status}")
    check_askedin_gateway_stand_in(subject, token)
    # New kids are unknown to the gateway's cache: it must refetch the key set, not refuse.
    # It refetches for an unseen kid only 10 s after its last fetch (kUnseenKidFetchSpacing in
    # infra/auth/src/jwks_verifier.cpp), and this scenario's first requests just made one; a
    # real issuer publishes a key well before it signs with it. The gateway is a black box
    # here, with nothing to make it fetch sooner, so the test waits the spacing out.
    status, _, data = request("POST", "/mock-auth/rotate")
    check(status == 200, f"rotate: {status} {data!r}")
    time.sleep(11)
    status, _, _ = request("GET", missing, mint(subject, "EdDSA"))
    check(status == 404, f"token from rotated keys: expected 404, got {status}")
    status, _, _ = request("GET", missing, token)
    check(status == 404, f"token from the previous keys: expected 404, got {status}")


def scenario_upload(workdir):
    data = make_clip(workdir / "short.mp4", 6, "2M")
    token = mint(f"up-{uuid.uuid4().hex[:8]}")
    upload = create_upload(token, "short.mp4", len(data))
    check(send_from(token, upload["upload_id"], data, 0, upload["chunk_size"]) == len(data),
          "upload ended short")
    state = commit_and_wait_ready(token, upload)
    print(f"  video {upload['video_id']} ready, {len(data)} bytes, {state['duration_ms']} ms")


def gateway_pods():
    out = kubectl("-n", NAMESPACE, "get", "pods", "-l", "app.kubernetes.io/name=video-gateway",
                  "-o", "jsonpath={range .items[*]}{.metadata.name}{\"\\n\"}{end}").stdout
    return [p for p in out.split() if p]


def uploads_in_flight(pod):
    # Through the API server's pod proxy, which is how the kubelet-side of the cluster reads it
    # too; /metrics is not routed through Envoy.
    result = kubectl("get", "--raw", f"/api/v1/namespaces/{NAMESPACE}/pods/{pod}:8080/proxy/metrics",
                     check_rc=False)
    match = re.search(r"^uploads_in_flight (\d+)$", result.stdout, re.M)
    return int(match.group(1)) if match else None


def serving_pods():
    """The gateway pod holding the one upload in flight, or every gateway pod when their
    metrics are out of reach (a CNI enforcing the NetworkPolicy may refuse the API server's
    proxy when it runs on another node)."""
    pods = gateway_pods()
    counts = {p: uploads_in_flight(p) for p in pods}
    if all(c is None for c in counts.values()):
        return pods
    serving = [p for p, c in counts.items() if c == 1]
    check(len(serving) == 1, f"expected one pod with the upload in flight, got {counts}")
    return serving


def scenario_pod_kill(workdir):
    data = make_clip(workdir / "long.mp4", 10, "20M")
    token = mint(f"kill-{uuid.uuid4().hex[:8]}")
    upload = create_upload(token, "long.mp4", len(data))
    upload_id, chunk = upload["upload_id"], upload["chunk_size"]
    check(len(data) > 2 * chunk, f"clip of {len(data)} bytes is too short to kill mid-upload")
    patch(token, upload_id, 0, data[:chunk])
    # The second chunk goes at 128 KiB/s, so its 8 MiB would take 64 s: longer than the
    # gateway's 30 s drain deadline, so the pod deleted a few seconds in must cut it off
    # rather than let it finish.
    rate, kill_after = 128 << 10, 1 << 20
    body = data[chunk:2 * chunk]
    conn = connect(120)
    conn.putrequest("PATCH", f"/api/v1/uploads/{upload_id}")
    conn.putheader("Authorization", f"Bearer {token}")
    conn.putheader("Upload-Offset", str(chunk))
    conn.putheader("Content-Type", "application/offset+octet-stream")
    conn.putheader("Content-Length", str(len(body)))
    conn.endheaders()
    victims, sent, started = [], 0, time.monotonic()
    try:
        while sent < len(body):
            piece = body[sent:sent + (16 << 10)]
            conn.send(piece)
            sent += len(piece)
            if not victims and sent >= kill_after:
                victims = serving_pods()
                kubectl("-n", NAMESPACE, "delete", "pod", *victims, "--wait=false")
                print(f"  deleted {' '.join(victims)} {sent >> 10} KiB into the second chunk")
            ahead = sent / rate - (time.monotonic() - started)
            if ahead > 0:
                time.sleep(ahead)
        outcome = f"HTTP {conn.getresponse().status}"
    except (OSError, http.client.HTTPException) as e:
        outcome = type(e).__name__
    conn.close()
    check(victims, "the chunk finished before a pod could be deleted")
    check(outcome != "HTTP 204", "the deleted pod still completed the chunk")
    resume_at = offset_of(token, upload_id)
    check(resume_at == chunk, f"resume offset {resume_at}, expected the first chunk's end {chunk}")
    print(f"  cut off after {time.monotonic() - started:.0f} s with {outcome}; "
          f"HEAD says {resume_at >> 20} MiB")
    check(send_from(token, upload_id, data, resume_at, chunk) == len(data),
          "resumed upload ended short")
    state = commit_and_wait_ready(token, upload)
    check(not set(victims) & set(gateway_pods()), f"{victims} still running")
    print(f"  video {upload['video_id']} ready after the resume, {state['duration_ms']} ms")


def scenario_netpol():
    pods = kubectl("-n", NAMESPACE, "get", "pods", "-l", "app.kubernetes.io/name=video-gateway",
                   "-o", "jsonpath={.items[0].status.podIP}").stdout.strip()
    check(pods, "no gateway pod IP")
    # bash's /dev/tcp opens a plain TCP connection; the probe needs no curl in the image.
    probe = (f"timeout 5 bash -c 'exec 3<>/dev/tcp/{pods}/8080 && "
             f"printf \"GET /api/v1/videos/{unknown_video_id()} HTTP/1.1\\r\\nHost: x\\r\\n"
             f"x-user-id: forged\\r\\nConnection: close\\r\\n\\r\\n\" >&3 && head -1 <&3' "
             "&& echo gateway-reached; "
             "timeout 5 bash -c 'exec 3<>/dev/tcp/mock-auth.auth.svc.cluster.local/80' "
             "&& echo auth-reached; true")
    result = kubectl("-n", NAMESPACE, "run", f"netpol-probe-{uuid.uuid4().hex[:6]}", "--rm", "-i",
                     "--restart=Never", "--image=ulw/video-gateway:e2e",
                     "--image-pull-policy=Never", "--command", "--", "bash", "-c", probe)
    check("auth-reached" in result.stdout, f"the probe pod has no network at all: {result.stdout!r}")
    if "gateway-reached" in result.stdout:
        # Even unfenced, the gateway must have ignored the header and asked for a token.
        check("401" in result.stdout.splitlines()[0],
              f"a forged x-user-id got past the gateway: {result.stdout!r}")
        raise Failure("a pod outside Envoy reached the gateway despite the NetworkPolicy; "
                      "is kube-router running in kube-system?")
    check_worker_egress(pods)


def check_worker_egress(gateway_ip):
    """From inside the worker, which runs ffmpeg on hostile input: the store it needs is reached,
    and what its egress policy denies is not. Nothing fences mock-auth's pods for ingress, and
    the probe pod above reaches its port 80, so only the worker's own policy keeps the worker off
    it. The gateway's ingress policy refuses the worker too, so that target is a second fence.
    Each connect is bounded by timeout(1): a denied one is dropped or refused, never answered."""
    auth = "mock-auth.auth.svc.cluster.local"
    denied = [f"{gateway_ip}:8080", f"{auth}:80"]
    probe = ("reach() { if timeout 5 bash -c \"exec 3<>/dev/tcp/$1/$2\" 2>/dev/null; "
             "then echo \"reached $1:$2\"; else echo \"refused $1:$2\"; fi; }; "
             f"getent hosts {auth} >/dev/null && echo resolved; "
             f"reach minio 9000; reach {gateway_ip} 8080; reach {auth} 80")
    result = kubectl("-n", NAMESPACE, "exec", "deploy/video-worker", "-c", "worker", "--",
                     "bash", "-c", probe)
    lines = set(result.stdout.splitlines())
    check("resolved" in lines, f"the worker cannot resolve {auth}: {result.stdout!r}")
    check("reached minio:9000" in lines,
          f"the worker cannot reach the store it needs: {result.stdout!r}")
    reached = [target for target in denied if f"refused {target}" not in lines]
    check(not reached, f"the worker reached {', '.join(reached)} despite its egress "
                       f"NetworkPolicy: {result.stdout!r}")
    print(f"  the worker reaches the store and is refused {', '.join(denied)}")


def fetch_from_store(url, origin):
    """GETs a presigned URL the way a player would, from the viewer's side; in the sandbox the
    store's name resolves to the port e2e-up.sh publishes it on."""
    parts = urllib.parse.urlsplit(url)
    host, port = SANDBOX_STORE.get(parts.netloc, (parts.hostname, parts.port))
    kind = http.client.HTTPSConnection if parts.scheme == "https" else http.client.HTTPConnection
    conn = kind(host, port, timeout=60)
    path = parts.path + (f"?{parts.query}" if parts.query else "")
    conn.request("GET", path, headers={"Host": parts.netloc, "Origin": origin})
    response = conn.getresponse()
    body = response.read()
    conn.close()
    return response.status, {k.lower(): v for k, v in response.getheaders()}, body


def playlist_uris(text):
    """Every URI a playlist names: its non-tag lines and the URI="..." of EXT-X-MAP."""
    uris = re.findall(r'^#EXT-X-MAP:.*URI="([^"]+)"', text, re.M)
    uris += [line for line in text.splitlines() if line and not line.startswith("#")]
    return uris


def scenario_playback(workdir):
    data = make_clip(workdir / "play.mp4", 6, "2M")
    subject = f"play-{uuid.uuid4().hex[:8]}"
    token = mint(subject)
    upload = create_upload(token, "play.mp4", len(data))
    send_from(token, upload["upload_id"], data, 0, upload["chunk_size"])
    commit_and_wait_ready(token, upload)
    master_path = f"/api/v1/videos/{upload['video_id']}/master.m3u8"
    status, _, _ = request("GET", master_path)
    check(status == 401, f"master without a token: expected 401, got {status}")
    if not TOKEN:
        status, _, _ = request("GET", master_path, mint(f"other-{subject}"))
        check(status == 404, f"master for another user: expected 404, got {status}")
    status, headers, body = request("GET", master_path, token)
    check(status == 200, f"master: {status} {body!r}")
    check(headers.get("cache-control") == "private, max-age=60",
          f"master Cache-Control: {headers.get('cache-control')!r}")
    master_url = urllib.parse.urlunsplit(BASE._replace(path=master_path))
    variants = [urllib.parse.urljoin(master_url, u) for u in playlist_uris(body.decode())]
    check(variants, f"master lists no variant: {body!r}")
    origin = "http://127.0.0.1:1"
    segments = 0
    for variant in variants:
        parts = urllib.parse.urlsplit(variant)
        check(parts.netloc == BASE.netloc,
              f"variant {variant} is not served by the gateway at {BASE.netloc}")
        status, _, media = request("GET", parts.path, token)
        check(status == 200, f"media playlist {parts.path}: {status} {media!r}")
        uris = playlist_uris(media.decode())
        check(uris and '#EXT-X-MAP:URI="' in media.decode(), f"{parts.path} has no init segment")
        for uri in uris:
            url = urllib.parse.urlsplit(uri)
            # Segment bytes never transit the gateway (ADR-0002): each URI is absolute, signed,
            # and on the store's host.
            check(url.scheme in ("http", "https") and url.netloc != BASE.netloc,
                  f"{parts.path} sends a segment through the gateway: {uri}")
            check("X-Amz-Signature=" in url.query, f"segment URL is not presigned: {uri}")
            status, seg_headers, seg = fetch_from_store(uri, origin)
            check(status == 200 and seg, f"segment {url.path}: {status}, {len(seg)} bytes")
            # ADR-0028: segments are fetched cross-origin, without credentials.
            check(seg_headers.get("access-control-allow-origin") in ("*", origin),
                  f"segment {url.path} has no CORS grant: {seg_headers}")
            segments += 1
    print(f"  {len(variants)} renditions, {segments} init and media segments from the store")


class ChatSocket:
    """A minimal RFC 6455 client for the chat scenario: masked text frames out, Pings answered,
    text frames in as JSON (tests/soak/chat_soak.py has the full one)."""

    def __init__(self, token=None):
        self.sock = socket.create_connection((BASE.hostname, BASE.port), timeout=10)
        key = base64.b64encode(os.urandom(16)).decode()
        head = (f"GET /rt HTTP/1.1\r\nHost: {BASE.hostname}:{BASE.port}\r\n"
                "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n")
        if token:
            head += f"Authorization: Bearer {token}\r\n"
        self.sock.sendall((head + "\r\n").encode())
        data = b""
        while b"\r\n\r\n" not in data:
            chunk = self.sock.recv(4096)
            if not chunk:
                break
            data += chunk
        line, _, self.buf = data.partition(b"\r\n\r\n")
        self.buf = bytearray(self.buf)
        parts = line.split(b" ", 2)
        self.status = int(parts[1]) if len(parts) > 1 else 0

    def send(self, obj, opcode=0x1):
        payload = obj if isinstance(obj, bytes) else json.dumps(obj).encode()
        n, mask = len(payload), os.urandom(4)
        head = bytes([0x80 | opcode])
        head += bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + n.to_bytes(2, "big")
        self.sock.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))

    def recv(self, timeout):
        """The next JSON message, or None when `timeout` passes first."""
        deadline = time.monotonic() + timeout
        while True:
            b = self.buf
            if len(b) >= 2:
                n, at = b[1] & 0x7F, 2
                if n == 126:
                    n, at = int.from_bytes(b[2:4], "big"), 4
                elif n == 127:
                    n, at = int.from_bytes(b[2:10], "big"), 10
                if len(b) >= at + n:
                    op, payload = b[0] & 0x0F, bytes(b[at:at + n])
                    del b[:at + n]
                    if op == 0x9:
                        self.send(payload, 0xA)
                    elif op == 0x8:
                        raise Failure(f"chat closed the socket: {payload[:2].hex()}")
                    elif op == 0x1:
                        return json.loads(payload)
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
                raise Failure("chat closed the connection")
            self.buf += chunk

    def expect(self, kind, timeout=10):
        while True:
            message = self.recv(timeout)
            check(message is not None, f"no {kind} from chat within {timeout} s")
            if message.get("type") == kind:
                return message
            check(message.get("type") != "error", f"chat answered {message} waiting for {kind}")

    def close(self):
        self.sock.close()


def scenario_chat():
    ready = kubectl("-n", NAMESPACE, "get", "deploy", "chat",
                    "-o", "jsonpath={.status.readyReplicas}").stdout.strip()
    check(ready == "3", f"chat has {ready or 0} ready replicas, not the overlay's 3")
    refused = ChatSocket()
    refused.close()
    check(refused.status == 401, f"an upgrade without a token: expected 401, got {refused.status}")
    # Opened as the RUNBOOK opens a stream's chat before its viewers arrive.
    stream = f"e2e-chat-{uuid.uuid4().hex[:8]}"
    kind = sandbox_sql(
        "INSERT INTO chat_rooms (room_id, kind) "
        f"SELECT live_chat_room('{stream}'), 'stream_live_chat' "
        f"WHERE NOT EXISTS (SELECT 1 FROM chat_members WHERE room_id = live_chat_room('{stream}')) "
        "AND NOT EXISTS (SELECT 1 FROM room_state "
        f"WHERE room_id = live_chat_room('{stream}') AND kind <> 'stream_live_chat') "
        "ON CONFLICT (room_id) DO UPDATE SET kind = chat_rooms.kind RETURNING kind")
    check(kind == "stream_live_chat", f"opening the stream's chat printed {kind!r}")
    # Enough sockets that Envoy spreads them over the three nodes, so the message crosses the
    # node channel from its room's owner to the others.
    sockets = []
    try:
        for i in range(6):
            s = ChatSocket(mint(f"chat-{i}-{uuid.uuid4().hex[:6]}", "RS256"))
            sockets.append(s)
            check(s.status == 101, f"upgrade {i} with a token: expected 101, got {s.status}")
            s.send({"type": "join", "stream": stream})
            room = s.expect("joined")["room"]
        body = base64.urlsafe_b64encode(os.urandom(24)).rstrip(b"=").decode()
        message_id = uuid.uuid4().hex
        sockets[0].send({"type": "send", "room": room, "id": message_id, "body": body})
        for i, s in enumerate(sockets):
            # The sender gets its own message as well as `sent`, in either order.
            wanted = {"message", "sent"} if i == 0 else {"message"}
            while wanted:
                got = s.recv(10)
                check(got is not None, f"socket {i}: no {' or '.join(sorted(wanted))} in 10 s")
                kind = got.get("type")
                check(kind != "error", f"socket {i}: chat answered {got}")
                if kind == "message":
                    check(got.get("id") == message_id and got.get("body") == body,
                          f"socket {i} got {got}, not the message sent")
                wanted.discard(kind)
    finally:
        for s in sockets:
            s.close()
    print(f"  {len(sockets)} sockets on the stream's chat each got the message")
    check_chat_node_port()


def check_chat_node_port():
    """The node channel admits chat pods alone (ADR-0035): a pod beside chat is refused there,
    though it reaches the auth-service, as in the netpol scenario."""
    ip = kubectl("-n", NAMESPACE, "get", "pods", "-l", "app.kubernetes.io/name=chat",
                 "-o", "jsonpath={.items[0].status.podIP}").stdout.strip()
    check(ip, "no chat pod IP")
    probe = (f"timeout 5 bash -c 'exec 3<>/dev/tcp/{ip}/9201' && echo node-reached; "
             "timeout 5 bash -c 'exec 3<>/dev/tcp/mock-auth.auth.svc.cluster.local/80' "
             "&& echo auth-reached; true")
    result = kubectl("-n", NAMESPACE, "run", f"chat-probe-{uuid.uuid4().hex[:6]}", "--rm", "-i",
                     "--restart=Never", "--image=ulw/video-gateway:e2e",
                     "--image-pull-policy=Never", "--command", "--", "bash", "-c", probe)
    check("auth-reached" in result.stdout,
          f"the probe pod has no network at all: {result.stdout!r}")
    check("node-reached" not in result.stdout,
          "a pod that is not chat reached chat's node port despite its NetworkPolicy")
    print("  a pod beside chat is refused its node port")


SCENARIOS = {
    "auth": lambda _: scenario_auth(),
    "upload": scenario_upload,
    "pod-kill": scenario_pod_kill,
    "netpol": lambda _: scenario_netpol(),
    "playback": scenario_playback,
    "chat": lambda _: scenario_chat(),
}


def main(argv):
    global BASE, TOKEN, KUBECTL_ALLOWED
    allow_skip = "--allow-skip" in argv
    names = [a for a in argv if a != "--allow-skip"]
    try:
        names, url, TOKEN = plan(names, os.environ)
        if url == SANDBOX_URL:
            require_sandbox(SANDBOX_KUBECONFIG)
            KUBECTL_ALLOWED = True
    except Refused as e:
        sys.exit(f"vod_flow: refusing: {e}")
    BASE = urllib.parse.urlsplit(url)
    failed, skipped = [], []
    with tempfile.TemporaryDirectory() as tmp:
        for name in names:
            print(f"{name}:")
            started = time.monotonic()
            try:
                SCENARIOS[name](pathlib.Path(tmp))
                print(f"  ok ({time.monotonic() - started:.0f} s)")
            except Skipped as e:
                skipped.append(name)
                print(f"  skipped: {e}")
            except Failure as e:
                failed.append(name)
                print(f"  FAILED: {e}")
    if skipped and not allow_skip:
        failed += [f"{n} (skipped)" for n in skipped]
    if failed:
        sys.exit(f"failed: {', '.join(failed)}")


if __name__ == "__main__":
    main(sys.argv[1:])
