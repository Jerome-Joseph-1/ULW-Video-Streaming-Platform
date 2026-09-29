"""The ordinary client slowloris.py and flood.py run beside their abusive traffic, to show it is
not degraded: it creates one small upload with its own token, then keeps reading that video's
state (GET /api/v1/videos/{id}, an authenticated catalog lookup that answers 200 through the
same route the abusive traffic hits). Latency and error rate come back as a summary.

start_uploads() is the same idea for the upload path: one upload after another, each sent as
fast as the gateway takes it, timed from create to the PATCH's 204."""
import http.client
import json
import statistics
import sys
import threading
import time
import urllib.parse


def connect(parts, timeout):
    kind = http.client.HTTPSConnection if parts.scheme == "https" else http.client.HTTPConnection
    return kind(parts.hostname, parts.port, timeout=timeout)


def create_video(url, token):
    parts = urllib.parse.urlsplit(url)
    conn = connect(parts, 10)
    try:
        conn.request("POST", "/api/v1/uploads",
                     body=json.dumps({"filename": "legit.mp4", "size_bytes": 1024,
                                      "content_type": "video/mp4"}),
                     headers={"Authorization": f"Bearer {token}",
                              "Content-Type": "application/json"})
        response = conn.getresponse()
        data = response.read()
    finally:
        conn.close()
    if response.status != 201:
        sys.exit(f"legit client: creating its video answered {response.status}; "
                 "it needs a valid --legit-token")
    return json.loads(data)["video_id"]


def summarize(latencies, errors):
    data, err = sorted(latencies), list(errors)
    summary = {
        "requests": len(data) + len(err),
        "errors": len(err),
        "error_rate": round(len(err) / (len(data) + len(err)), 4) if (data or err) else None,
        "errors_by_kind": {kind: err.count(kind) for kind in sorted(set(err))},
    }
    if data:
        qs = statistics.quantiles(data, n=100, method="inclusive") if len(data) > 1 else [data[0]] * 99
        summary["p50_ms"] = round((qs[49] if len(data) > 1 else data[0]) * 1000, 2)
        summary["p99_ms"] = round((qs[98] if len(data) > 1 else data[0]) * 1000, 2)
        summary["max_ms"] = round(data[-1] * 1000, 2)
    return summary


def start(url, token, duration):
    """Starts the client and returns stop(), which ends it and returns its summary."""
    del duration  # the caller stops it when the abusive traffic ends
    video_id = create_video(url, token)
    stop_event = threading.Event()
    latencies, errors = [], []
    lock = threading.Lock()

    def loop():
        parts = urllib.parse.urlsplit(url)
        while not stop_event.is_set():
            started = time.monotonic()
            try:
                conn = connect(parts, 10)
                conn.request("GET", f"/api/v1/videos/{video_id}",
                             headers={"Authorization": f"Bearer {token}"})
                response = conn.getresponse()
                response.read()
                conn.close()
                elapsed = time.monotonic() - started
                with lock:
                    latencies.append(elapsed)
                    if response.status != 200:
                        errors.append(f"http_{response.status}")
            except OSError as e:
                with lock:
                    errors.append(type(e).__name__)
            stop_event.wait(0.2)

    thread = threading.Thread(target=loop, daemon=True)
    thread.start()

    def stop():
        stop_event.set()
        thread.join(timeout=10)
        with lock:
            return summarize(latencies, errors)

    return stop


def upload_once(parts, token, body, timeout):
    """One whole upload on one connection, cancelled once its bytes are durable so that a long
    run leaves nothing in the store; raises OSError on a timeout or a dropped socket."""
    conn = connect(parts, timeout)
    try:
        conn.request("POST", "/api/v1/uploads",
                     body=json.dumps({"filename": "legit.mp4", "size_bytes": len(body),
                                      "content_type": "video/mp4"}),
                     headers={"Authorization": f"Bearer {token}",
                              "Content-Type": "application/json"})
        response = conn.getresponse()
        data = response.read()
        if response.status != 201:
            return response.status
        path = f"/api/v1/uploads/{json.loads(data)['upload_id']}"
        conn.request("PATCH", path, body=body,
                     headers={"Authorization": f"Bearer {token}", "Upload-Offset": "0",
                              "Content-Type": "application/offset+octet-stream"})
        response = conn.getresponse()
        response.read()
        status = response.status
        conn.request("DELETE", path, headers={"Authorization": f"Bearer {token}"})
        conn.getresponse().read()
        return status
    finally:
        conn.close()


def start_uploads(url, token, size, timeout):
    """Uploads `size` bytes, one upload after another, until stop(); an upload that takes longer
    than `timeout` seconds without the gateway answering counts as an error."""
    body = bytes(size)
    stop_event = threading.Event()
    latencies, errors = [], []
    lock = threading.Lock()

    # When the upload in progress started: one still waiting at stop() is reported with its age.
    current = {"started": None}

    def loop():
        parts = urllib.parse.urlsplit(url)
        while not stop_event.is_set():
            started = time.monotonic()
            with lock:
                current["started"] = started
            try:
                status = upload_once(parts, token, body, timeout)
                with lock:
                    if status == 204:
                        latencies.append(time.monotonic() - started)
                    else:
                        errors.append(f"http_{status}")
            except (OSError, http.client.HTTPException) as e:
                with lock:
                    errors.append(type(e).__name__)
            # One upload a second: a steady user, not a load of its own.
            stop_event.wait(1.0)

    thread = threading.Thread(target=loop, daemon=True)
    thread.start()

    def stop():
        stop_event.set()
        thread.join(timeout=1)
        with lock:
            summary = summarize(latencies, errors)
            if thread.is_alive() and current["started"] is not None:
                summary["unanswered_upload_age_s"] = round(time.monotonic() - current["started"], 1)
        return summary

    return stop
