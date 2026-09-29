"""The ordinary client slowloris.py and flood.py run beside their abusive traffic, to show it is
not degraded: it creates one small upload with its own token, then keeps reading that video's
state (GET /api/v1/videos/{id}, an authenticated catalog lookup that answers 200 through the
same route the abusive traffic hits). Latency and error rate come back as a summary."""
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
            data, err = sorted(latencies), list(errors)
        summary = {
            "requests": len(data) + len(err),
            "errors": len(err),
            "error_rate": round(len(err) / (len(data) + len(err)), 4) if (data or err) else None,
        }
        if data:
            qs = statistics.quantiles(data, n=100) if len(data) > 1 else [data[0]] * 99
            summary["p50_ms"] = round((qs[49] if len(data) > 1 else data[0]) * 1000, 2)
            summary["p99_ms"] = round((qs[98] if len(data) > 1 else data[0]) * 1000, 2)
        return summary

    return stop
