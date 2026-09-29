#!/usr/bin/env python3
"""Resource check under load, in the sandbox cluster (deploy/local/e2e-up.sh, then
deploy/local/metrics-server.sh): M14's "kubectl top stays within the derived limits under 500
concurrent uploads".

    tests/cluster/load_check.py [--uploads 500] [--ready-wait 120] [--out DIR]

Uploads go through Envoy's HTTPRoute with tokens minted by the mock auth-service, driven by
tests/load/upload_load.py, each holding one connection open and sending a real MP4 the worker
transcodes; how many videos reached ready is reported, not required. While they run, `kubectl top pods --containers` is sampled every 10 s
for the video-gateway and video-worker pods. The run fails if any sample exceeds a container's
CPU or memory limit as the applied Deployment declares it (the stage overlay, which is what
deploy/askedin/overlays ships), if a container restarted or was OOM-killed (a killed pod has no
sample to show it), or if fewer uploads were in flight at the peak than the gateway admits.
Writes report.json and samples.csv to --out.

Like vod_flow.py, kubectl only ever runs against the sandbox: deploy/local/.state/kubeconfig with
context kind-ulw-e2e, whose API server must be on 127.0.0.1, checked before anything runs.
"""
import argparse
import csv
import json
import math
import pathlib
import re
import subprocess
import sys
import tempfile
import threading
import time
import uuid

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "load"))
import upload_load  # noqa: E402
import vod_flow  # noqa: E402

ROOT = vod_flow.ROOT
NAMESPACE = vod_flow.NAMESPACE
SAMPLE_INTERVAL_S = 10
# The gauges are cheap to read and move fast: once the first uploads finish, the ones waiting
# for a store connection drain within seconds, and a 10 s poll could miss the peak.
GAUGE_INTERVAL_S = 2
# The clip every upload sends: 4 s of 1280x720 at 2 Mbit/s is 1.06 MiB, which SAFE_RATE holds
# open for 68 s, and a real transcode for the worker, as vod_flow's upload scenario has.
CLIP_SECONDS, CLIP_BITRATE = 4, "2M"
# apps/gateway/src/gateway.hpp Limits::max_upload_slots: the most uploads the gateway holds at
# once, per pod; further creates are refused with 503.
GATEWAY_UPLOAD_SLOTS = 448
# The gateway admits 3 uploads per user (Limits::max_uploads_per_user); two per user leaves
# room for an upload whose commit lags, so no create is refused with 429.
UPLOADS_PER_USER = 2
# Most of the admitted uploads must be in flight together at the peak: the check is void if the
# driver ramped too slowly or the route dropped them. The gateway streams only as many chunk
# bodies as the store has connections (64); the rest wait for one, held back rather than
# failed (docs/adr/0033), so the peak lasts until the first uploads finish.
MIN_PEAK_FRACTION = 0.9
WORKLOADS = ("video-gateway", "video-worker")

CPU = re.compile(r"^(\d+(?:\.\d+)?)(m?)$")
MEMORY = re.compile(r"^(\d+(?:\.\d+)?)(Ki|Mi|Gi|Ti|k|M|G|T)?$")
MEMORY_UNITS = {None: 1, "Ki": 1 << 10, "Mi": 1 << 20, "Gi": 1 << 30, "Ti": 1 << 40,
                "k": 10 ** 3, "M": 10 ** 6, "G": 10 ** 9, "T": 10 ** 12}


Failure = vod_flow.Failure


def cpu_millicores(text):
    match = CPU.match(text)
    if not match:
        raise Failure(f"unparsable CPU quantity {text!r}")
    value = float(match.group(1))
    return round(value if match.group(2) else value * 1000)


def memory_bytes(text):
    match = MEMORY.match(text)
    if not match:
        raise Failure(f"unparsable memory quantity {text!r}")
    return round(float(match.group(1)) * MEMORY_UNITS[match.group(2)])


def limits_of(deployments):
    """{(workload, container): {"cpu_m": n | None, "memory": bytes | None}} for the containers
    of a `kubectl get deployments -o json` document; init containers run and end before load."""
    limits = {}
    for item in deployments["items"]:
        workload = item["metadata"]["name"]
        for container in item["spec"]["template"]["spec"]["containers"]:
            declared = (container.get("resources") or {}).get("limits") or {}
            limits[(workload, container["name"])] = {
                "cpu_m": cpu_millicores(declared["cpu"]) if "cpu" in declared else None,
                "memory": memory_bytes(declared["memory"]) if "memory" in declared else None,
            }
    return limits


def parse_top(output):
    """Rows of `kubectl top pods --containers --no-headers`: pod, container, CPU, memory."""
    rows = []
    for line in output.splitlines():
        fields = line.split()
        if len(fields) != 4:
            raise Failure(f"unexpected kubectl top line {line!r}")
        rows.append({"pod": fields[0], "container": fields[1],
                     "cpu_m": cpu_millicores(fields[2]), "memory": memory_bytes(fields[3])})
    return rows


def workload_of(pod):
    for workload in WORKLOADS:
        if pod.startswith(workload + "-"):
            return workload
    return None


def violations(samples, limits):
    """Every sample that exceeds its container's limit, as readable lines."""
    found = []
    for sample in samples:
        for row in sample["containers"]:
            workload = workload_of(row["pod"])
            declared = limits.get((workload, row["container"]))
            if declared is None:
                continue
            if declared["memory"] is not None and row["memory"] > declared["memory"]:
                found.append(f"t={sample['t']:.0f}s {row['pod']}/{row['container']} memory "
                             f"{row['memory'] >> 20} MiB > limit {declared['memory'] >> 20} MiB")
            if declared["cpu_m"] is not None and row["cpu_m"] > declared["cpu_m"]:
                found.append(f"t={sample['t']:.0f}s {row['pod']}/{row['container']} cpu "
                             f"{row['cpu_m']}m > limit {declared['cpu_m']}m")
    return found


def restarts():
    pods = json.loads(vod_flow.kubectl("-n", NAMESPACE, "get", "pods", "-o", "json").stdout)
    counts = {}
    for pod in pods["items"]:
        for status in pod["status"].get("containerStatuses") or []:
            terminated = (status.get("lastState") or {}).get("terminated") or {}
            counts[(pod["metadata"]["name"], status["name"])] = (
                status["restartCount"], terminated.get("reason"))
    return counts


def restart_problems(before, after):
    """Containers whose samples stop where a kill happened. A pod replaced during the run has
    a new name and no restarts, so it is caught by its predecessor's absence. A last state of
    OOMKilled counts only when restartCount rose during the run: one left over from before it
    says nothing about this load."""
    problems = []
    for key in before:
        if key not in after:
            problems.append(f"{key[0]}/{key[1]} is gone: the pod was replaced during the run")
    for key, (count, reason) in after.items():
        was = before.get(key, (0, None))[0]
        if count != was:
            problems.append(f"{key[0]}/{key[1]} restarted ({was} -> {count}, last exit "
                            f"{reason}): its samples stop where the kill happened")
    return problems


def peak_problems(peak_in_flight, admitted):
    if peak_in_flight is None:
        return ["could not read uploads_in_flight from any gateway pod; the check is void"]
    if peak_in_flight < MIN_PEAK_FRACTION * admitted:
        return [f"peak uploads in flight {peak_in_flight}, expected at least "
                f"{MIN_PEAK_FRACTION * admitted:.0f} of the {admitted} the gateway admits"]
    return []


def coverage_problems(samples):
    counts = {w: sum(1 for s in samples for r in s["containers"] if workload_of(r["pod"]) == w)
              for w in WORKLOADS}
    return [f"no sample of a {w} pod" for w, n in counts.items() if n == 0]


def top_rows():
    result = vod_flow.kubectl("-n", NAMESPACE, "top", "pods", "--containers", "--no-headers",
                              check_rc=False)
    if result.returncode != 0:
        return None
    return [r for r in parse_top(result.stdout) if workload_of(r["pod"])]


def wait_for_metrics():
    """metrics-server serves nothing until its first scrape, about a minute after it starts."""
    deadline = time.monotonic() + 180
    while time.monotonic() < deadline:
        rows = top_rows()
        if rows:
            return rows
        time.sleep(5)
    raise Failure("kubectl top returned nothing for 180 s; is metrics-server installed "
                  "(deploy/local/metrics-server.sh) and are the pods running?")


class Sampler(threading.Thread):
    def __init__(self):
        super().__init__(daemon=True)
        self.stop_event = threading.Event()
        self.samples = []
        self.gauges = []
        self.error = None
        self.started = time.monotonic()

    def run(self):
        ticks = SAMPLE_INTERVAL_S // GAUGE_INTERVAL_S
        tick = 0
        try:
            while not self.stop_event.is_set():
                self.read_gauges()
                if tick % ticks == 0:
                    self.take()
                tick += 1
                self.stop_event.wait(GAUGE_INTERVAL_S)
        except Exception as e:  # noqa: BLE001 - handed to finish(), which the run calls
            self.error = e

    def take(self):
        rows = top_rows()
        if rows is None:
            return
        gauges = self.gauges[-1] if self.gauges else {}
        self.samples.append({"t": time.monotonic() - self.started, "containers": rows,
                             "uploads_in_flight": gauges.get("uploads_in_flight")})

    def read_gauges(self):
        totals, seen = {"uploads_in_flight": 0, "connections_current": 0}, False
        for pod in vod_flow.gateway_pods():
            text = vod_flow.kubectl(
                "get", "--raw", f"/api/v1/namespaces/{NAMESPACE}/pods/{pod}:8080/proxy/metrics",
                check_rc=False).stdout
            for name in totals:
                match = re.search(rf"^{name} (\d+)$", text, re.M)
                if match:
                    totals[name] += int(match.group(1))
                    seen = True
        if seen:
            self.gauges.append({"t": time.monotonic() - self.started, **totals})

    def finish(self):
        self.stop_event.set()
        self.join()
        if self.error is not None:
            raise Failure(f"sampler stopped early: {type(self.error).__name__}: {self.error}")


def mint_tokens(users, path):
    with open(path, "w", encoding="utf-8") as out:
        run = uuid.uuid4().hex[:6]
        for i in range(users):
            out.write(vod_flow.mint(f"load-{run}-{i:04d}") + "\n")


def write_reports(out, report, samples):
    out.mkdir(parents=True, exist_ok=True)
    (out / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    with open(out / "samples.csv", "w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(["t_s", "pod", "container", "cpu_millicores", "memory_bytes",
                         "uploads_in_flight"])
        for sample in samples:
            for row in sample["containers"]:
                writer.writerow([f"{sample['t']:.1f}", row["pod"], row["container"],
                                 row["cpu_m"], row["memory"], sample["uploads_in_flight"]])


def peaks(samples):
    peak = {}
    for sample in samples:
        for row in sample["containers"]:
            key = f"{row['pod']}/{row['container']}"
            best = peak.setdefault(key, {"cpu_m": 0, "memory": 0})
            best["cpu_m"] = max(best["cpu_m"], row["cpu_m"])
            best["memory"] = max(best["memory"], row["memory"])
    return peak


def count_ready(committed, tokens, wait_s):
    """How many of the committed videos reached ready within wait_s; the worker has limited CPU
    and is not expected to finish all of them, so this is reported, never required."""
    states = {}
    deadline = time.monotonic() + wait_s
    while True:
        for video in committed:
            if states.get(video["video_id"]) in ("ready", "failed"):
                continue
            status, _, data = vod_flow.request(
                "GET", f"/api/v1/videos/{video['video_id']}", tokens[video["token_index"]])
            states[video["video_id"]] = json.loads(data)["state"] if status == 200 else f"http {status}"
        pending = [v for v in states.values() if v not in ("ready", "failed")]
        if not pending or time.monotonic() >= deadline:
            break
        time.sleep(5)
    counts = {}
    for state in states.values():
        counts[state] = counts.get(state, 0) + 1
    return counts


def build_report(args, users, size, rate, admitted, limits, sampler, load, videos, problems):
    gauges = sampler.gauges
    return {
        "uploads": args.uploads, "users": users, "size_bytes": size, "rate_bytes_per_s": rate,
        "sample_interval_s": SAMPLE_INTERVAL_S, "samples": len(sampler.samples),
        "peak_uploads_in_flight": max((g["uploads_in_flight"] for g in gauges), default=None),
        "peak_connections": max((g["connections_current"] for g in gauges), default=None),
        "admitted_expected": admitted, "videos": videos,
        "limits": {f"{w}/{c}": v for (w, c), v in limits.items()},
        "peaks": peaks(sampler.samples),
        "upload_load": None if load is None else {
            k: load[k] for k in ("attempted", "completed", "status_counts", "errors_by_kind",
                                 "wall_time_s", "patch_latency")},
        "violations": problems, "passed": not problems,
    }


def run(args):
    vod_flow.require_sandbox(vod_flow.SANDBOX_KUBECONFIG)
    vod_flow.KUBECTL_ALLOWED = True
    deployments = json.loads(vod_flow.kubectl(
        "-n", NAMESPACE, "get", "deployments", *WORKLOADS, "-o", "json").stdout)
    limits = limits_of(deployments)
    wait_for_metrics()
    restarts_before = restarts()

    rate = upload_load.SAFE_RATE
    users = math.ceil(args.uploads / UPLOADS_PER_USER)
    admitted = min(args.uploads, GATEWAY_UPLOAD_SLOTS * len(vod_flow.gateway_pods()))
    sampler, load, videos, size, problems = Sampler(), None, {}, 0, []
    # Whatever went wrong, the samples taken so far are the evidence: the report is written
    # from them before the failure is raised.
    try:
        with tempfile.TemporaryDirectory() as tmp:
            clip = pathlib.Path(tmp, "load.mp4")
            size = len(vod_flow.make_clip(clip, CLIP_SECONDS, CLIP_BITRATE))
            token_file = pathlib.Path(tmp, "tokens")
            mint_tokens(users, token_file)
            tokens = token_file.read_text(encoding="utf-8").split()
            sampler.start()
            print(f"  {args.uploads} uploads, {users} users, {size} bytes each at {rate} B/s")
            driver = subprocess.run(
                [sys.executable, str(ROOT / "tests/load/upload_load.py"), "--url",
                 vod_flow.SANDBOX_URL, "--token-file", str(token_file), "--uploads",
                 str(args.uploads), "--payload", str(clip), "--rate", str(rate)],
                capture_output=True, text=True)
            if driver.returncode != 0:
                raise Failure(f"upload_load.py exited {driver.returncode}: {driver.stderr.strip()}")
            load = json.loads(driver.stdout)
            videos = count_ready(load["committed_videos"], tokens, args.ready_wait)
            print(f"  videos after {args.ready_wait} s at most: {videos}")
    except Exception as e:  # noqa: BLE001 - recorded in the report, then raised below
        problems.append(f"{type(e).__name__}: {e}")
    if sampler.ident is not None:
        try:
            sampler.finish()
        except Failure as e:
            problems.append(str(e))
        sampler.take()

    problems += violations(sampler.samples, limits)
    problems += restart_problems(restarts_before, restarts())
    problems += peak_problems(
        max((g["uploads_in_flight"] for g in sampler.gauges), default=None), admitted)
    problems += coverage_problems(sampler.samples)
    if load is not None and load["completed"] == 0:
        problems.append("no upload completed")

    report = build_report(args, users, size, rate, admitted, limits, sampler, load, videos,
                          problems)
    write_reports(args.out, report, sampler.samples)
    for name, peak in report["peaks"].items():
        print(f"  {name}: peak {peak['cpu_m']}m, {peak['memory'] >> 20} MiB")
    print(f"  peak uploads in flight {report['peak_uploads_in_flight']}, connections "
          f"{report['peak_connections']}; reports in {args.out}")
    if problems:
        raise Failure("; ".join(problems))


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--uploads", type=int, default=500)
    parser.add_argument("--ready-wait", type=int, default=120,
                        help="seconds to wait, after the uploads end, for the videos to be ready")
    parser.add_argument("--out", type=pathlib.Path, default=ROOT / "load-report")
    args = parser.parse_args(argv)
    try:
        print("load:")
        run(args)
        print("  ok")
    except vod_flow.Refused as e:
        sys.exit(f"load_check: refusing: {e}")
    except Failure as e:
        sys.exit(f"load_check: FAILED: {e}")


if __name__ == "__main__":
    main(sys.argv[1:])
