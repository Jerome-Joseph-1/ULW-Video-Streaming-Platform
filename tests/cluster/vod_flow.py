#!/usr/bin/env python3
"""End-to-end checks of the VOD plane in the sandbox cluster (deploy/local/e2e-up.sh).

Every request goes through Envoy's HTTPRoute at 127.0.0.1:18080 with a token minted by the
mock auth-service, as a browser's would through askedin-gateway:

  auth        each signing algorithm, the cookie, key rotation, and refusals
  upload      a clip uploaded chunk by chunk, transcoded by the worker, reaching ready
  pod-kill    the gateway pod serving an upload deleted mid-chunk; the chunk is cut off, the
              client asks HEAD for the durable offset, finishes against the pods left, and the
              video reaches ready
  netpol      a pod beside the gateway cannot open a connection to it (x-user-id forgery has
              nothing to reach), while it can reach the auth-service
  playback    placeholder until playback exists (M11)

    tests/cluster/vod_flow.py [SCENARIO...]     all of them when none is named

Needs ffmpeg on PATH for the test clips and KUBECONFIG pointing at the sandbox (e2e-up.sh
leaves it in deploy/local/.state/kubeconfig).

Against a real deployment (deploy/askedin/RUNBOOK.md), ULW_E2E_URL names the Gateway
(https://host) and ULW_E2E_TOKEN a token its auth-service issued; only upload and pod-kill
apply there, since the others drive the mock auth-service and the kind node.
"""
import http.client
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import time
import urllib.parse
import uuid

BASE = urllib.parse.urlsplit(os.environ.get("ULW_E2E_URL", "http://127.0.0.1:18080"))
TOKEN = os.environ.get("ULW_E2E_TOKEN")
NAMESPACE = "apps-stage"
NODE = "ulw-e2e-control-plane"
ROOT = pathlib.Path(__file__).resolve().parents[2]
KUBECTL = str(ROOT / "deploy/local/.tools/kubectl")
# A clip at 1280x720 transcodes to two rungs (720p, 360p) in well under a minute on the
# sandbox's half-core worker; the clock allows for a slow machine.
READY_TIMEOUT_S = 600


class Failure(Exception):
    pass


class Skipped(Exception):
    pass


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
    result = subprocess.run([KUBECTL, *args], capture_output=True, text=True)
    if check_rc and result.returncode != 0:
        raise Failure(f"kubectl {' '.join(args)}: {result.stderr.strip()}")
    return result


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


def scenario_auth():
    subject = f"auth-{uuid.uuid4().hex[:8]}"
    missing = f"/api/v1/videos/{unknown_video_id()}"
    for alg in ("RS256", "ES256", "EdDSA"):
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
    # New kids are unknown to the gateway's cache: it must refetch the key set, not refuse.
    # It refetches for an unseen kid only 10 s after its last fetch (kUnseenKidFetchSpacing in
    # infra/auth/src/jwks_verifier.cpp), and this scenario's first requests just made one; a
    # real issuer publishes a key well before it signs with it.
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


def node_can_enforce_policies():
    """kindnet enforces NetworkPolicy by queueing packets to user space (nftables `queue`);
    some kernels, like the nested VMs sandboxes often run on, are built without it."""
    probe = ("nft add table inet ulw_probe && nft add chain inet ulw_probe c && "
             "nft add rule inet ulw_probe c queue num 1 bypass; rc=$?; "
             "nft delete table inet ulw_probe; exit $rc")
    result = subprocess.run(["docker", "exec", NODE, "sh", "-c", probe], capture_output=True)
    return result.returncode == 0


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
        if not node_can_enforce_policies():
            raise Skipped("the node's kernel has no nftables queue support, which kindnet's "
                          "NetworkPolicy engine needs, so no policy is enforced here; the "
                          "gateway answered the forged x-user-id with 401")
        raise Failure("a pod outside Envoy reached the gateway despite the NetworkPolicy")


def scenario_playback():
    raise Skipped("playback through the route waits for M11 (hls.js via Playwright)")


SCENARIOS = {
    "auth": lambda _: scenario_auth(),
    "upload": scenario_upload,
    "pod-kill": scenario_pod_kill,
    "netpol": lambda _: scenario_netpol(),
    "playback": lambda _: scenario_playback(),
}


def main(names):
    unknown = [n for n in names if n not in SCENARIOS]
    if unknown:
        sys.exit(f"unknown scenario {unknown}; choose from {list(SCENARIOS)}")
    os.environ.setdefault("KUBECONFIG", str(ROOT / "deploy/local/.state/kubeconfig"))
    failed = []
    with tempfile.TemporaryDirectory() as tmp:
        for name in names or SCENARIOS:
            print(f"{name}:")
            started = time.monotonic()
            try:
                SCENARIOS[name](pathlib.Path(tmp))
                print(f"  ok ({time.monotonic() - started:.0f} s)")
            except Skipped as e:
                print(f"  skipped: {e}")
            except Failure as e:
                failed.append(name)
                print(f"  FAILED: {e}")
    if failed:
        sys.exit(f"failed: {' '.join(failed)}")


if __name__ == "__main__":
    main(sys.argv[1:])
