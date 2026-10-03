#!/usr/bin/env python3
"""soak.py's reports beside the verdict, on synthetic data: the RSS shape per hour and over the
last hours, the 5xx attribution against the deliberate faults' windows, the load paths that did
not run, and the check that the worker's sandbox can reach its scratch directory."""
import json
import os
import pathlib
import stat
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import soak  # noqa: E402
from soak import coverage_report, errors_report, judge, shape_report, unexercised  # noqa: E402

REQUESTS_PER_MIN = 1_500


def samples(minutes, rss_kb_at):
    return [{"elapsed_min": float(m), "gateway_rss_kb": rss_kb_at(m), "gateway_fds": 30,
             "worker_rss_kb": 14_000, "worker_fds": 4, "requests": REQUESTS_PER_MIN * m,
             "chunks": 180 * m, "upload_sessions": 20 * m, "uploads_committed": 5 * m}
            for m in range(minutes + 1)]


def line_with(lines, text):
    found = [line for line in lines if text in line]
    assert found, f"no line with {text!r} in {lines}"
    return found[0]


class ShapeReport(unittest.TestCase):
    def test_a_plateau_shows_as_no_slope_over_the_last_hours(self):
        # 2 MiB over the first two hours, then flat.
        lines = shape_report(samples(360, lambda m: 30_000 + min(m, 120) * 2048 // 120))
        self.assertIn("+0.0 KB/h", line_with(lines, "last 4 h"))
        self.assertIn("+0.0 KB/h", line_with(lines, "last 2 h"))
        # Marks at 0, 15, 30, 45 and 60 min, then every hour, and the last sample.
        self.assertEqual(len([line for line in lines if " h  " in line]), 10)

    def test_a_steady_tail_shows_its_rate(self):
        # 40 KB an hour throughout, in KiB a minute.
        lines = shape_report(samples(360, lambda m: 30_000 + int(m * 40_000 / 1024 / 60)))
        self.assertIn("slope +40.0 KB/h", line_with(lines, "last 4 h"))
        self.assertIn("slope +40.0 KB/h", line_with(lines, "last 2 h"))
        self.assertIn("B a request at most", line_with(lines, "last 2 h"))

    def test_a_short_run_says_so_rather_than_fitting_the_warm_up(self):
        lines = shape_report(samples(60, lambda m: 30_000))
        self.assertIn("too short", line_with(lines, "last 4 h"))
        self.assertIn("too short", line_with(lines, "last 2 h"))

    def test_the_verdict_is_the_one_judge_gives_without_the_report(self):
        rows = samples(360, lambda m: 30_000 + int(m * 40_000 / 1024 / 60))
        passed, _ = judge(rows, 8)
        self.assertFalse(passed)


class ErrorsReport(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.log = pathlib.Path(self.dir.name) / "gateway.log"

    def tearDown(self):
        self.dir.cleanup()

    def write_log(self, entries):
        # As the gateway writes it: compact, which the scan for "status":5 relies on.
        with open(self.log, "w") as f:
            for ts, route, status in entries:
                f.write(json.dumps({"ts": ts, "level": "warn", "event": "request",
                                    "method": "GET", "route": route, "status": status,
                                    "ms": 0}, separators=(",", ":")) + "\n")
            f.write('{"ts":"2026-10-03T08:00:00.000Z","event":"request","status":200}\n')

    def test_each_5xx_is_placed_in_the_fault_that_made_it_or_left_out_of_both(self):
        t0 = soak.utc_seconds("2026-10-03T08:00:00.000Z")
        windows = [("saturation", t0, t0 + 1), ("store_fault", t0 + 100, t0 + 100.5)]
        self.write_log([("2026-10-03T08:00:00.500Z", "append_chunk", 503),
                        ("2026-10-03T08:00:01.900Z", "append_chunk", 503),
                        ("2026-10-03T08:01:40.200Z", "media_playlist", 500),
                        ("2026-10-03T08:05:00.000Z", "get_video", 503)])
        totals = {"5xx saturation 503": 1, "5xx store_fault 500": 1, "status_5xx": 2}
        lines = errors_report(totals, windows, self.log)
        self.assertIn("  saturation 503: 1", lines)
        self.assertIn("  store_fault 500: 1", lines)
        self.assertIn("  append_chunk 503 during saturation: 2", lines)
        self.assertIn("  media_playlist 500 during store_fault: 1", lines)
        self.assertIn("  get_video 503 during outside both: 1", lines)
        self.assertIn("get_video 503", line_with(lines, "08:05:00"))

    def test_overlapping_faults_each_take_the_status_they_make(self):
        t0 = soak.utc_seconds("2026-10-03T08:00:00.000Z")
        windows = [("store_fault", t0, t0 + 3), ("saturation", t0 + 1, t0 + 2)]
        self.write_log([("2026-10-03T08:00:01.500Z", "append_chunk", 503),
                        ("2026-10-03T08:00:01.600Z", "media_playlist", 500)])
        lines = errors_report({}, windows, self.log)
        self.assertIn("  append_chunk 503 during saturation: 1", lines)
        self.assertIn("  media_playlist 500 during store_fault: 1", lines)

    def test_without_the_gateway_log_only_the_clients_are_reported(self):
        lines = errors_report({}, [], self.log)
        self.assertEqual(lines, ["5xx seen by the clients, by action and status:", "  none"])


class CoverageReport(unittest.TestCase):
    def test_a_run_whose_videos_all_failed_shows_what_did_not_run(self):
        totals = {"videos_failed": 3, "video_failed: transcoding failed": 3,
                  "store_fault_skipped_no_ready_video": 2, "resumes_completed": 4,
                  "cancels": 2}
        lines = coverage_report(totals)
        self.assertIn("NOT EXERCISED", line_with(lines, "videos made ready"))
        self.assertIn("NOT EXERCISED", line_with(lines, "media playlists fetched"))
        self.assertIn("NOT EXERCISED", line_with(lines, "store faults"))
        self.assertIn("NOT EXERCISED", line_with(lines, "saturations"))
        self.assertNotIn("NOT EXERCISED", line_with(lines, "videos failed"))
        self.assertIn("  video_failed: transcoding failed: 3", lines)
        self.assertIn("  store_fault_skipped_no_ready_video: 2", lines)


class Validity(unittest.TestCase):
    FULL = {"videos_ready_total": 5, "playlists": 9, "store_missing_500": 1,
            "uploads_committed": 5, "resumes_completed": 2, "cancels": 1,
            "saturation_total_503": 3, "slow_header_ended": 1}

    def test_a_run_that_exercised_every_path_is_valid(self):
        self.assertEqual(unexercised(self.FULL), [])

    def test_failed_videos_are_reported_but_not_required(self):
        self.assertEqual(unexercised({**self.FULL, "videos_failed": 0}), [])

    def test_each_required_path_at_zero_makes_the_run_invalid(self):
        for key, name in [("videos_ready_total", "videos made ready"),
                          ("playlists", "media playlists fetched"),
                          ("store_missing_500", "store faults that fetched a deleted playlist"),
                          ("saturation_total_503", "saturations refused by the gateway's limit"),
                          ("slow_header_ended", "slow clients ended by a timer")]:
            self.assertEqual(unexercised({**self.FULL, key: 0}), [name], key)

    def test_the_runs_on_the_runner_whose_probes_all_failed_are_invalid(self):
        # Run 37108394263's totals, abridged: no video ready, so no playlist or store fault.
        totals = {"cancels": 1755, "resumes_completed": 3480, "saturation_total_503": 348,
                  "slow_header_ended": 360, "uploads_committed": 1741}
        self.assertEqual(unexercised(totals), ["videos made ready", "media playlists fetched",
                                               "store faults that fetched a deleted playlist"])


class BucketCleanup(unittest.TestCase):
    """delete_bucket() against a fake store: every key on every listing page, then the bucket."""

    NS = "http://s3.amazonaws.com/doc/2006-03-01/"

    def page(self, keys, token=None):
        contents = "".join(f"<Contents><Key>{k}</Key></Contents>" for k in keys)
        more = f"<IsTruncated>true</IsTruncated><NextContinuationToken>{token}" \
               "</NextContinuationToken>" if token else "<IsTruncated>false</IsTruncated>"
        return f'<ListBucketResult xmlns="{self.NS}">{contents}{more}</ListBucketResult>'

    def test_every_object_on_every_page_goes_before_the_bucket(self):
        stack = soak.Stack.__new__(soak.Stack)
        stack.bucket = "ulw-soak-x"
        pages = {"ulw-soak-x?list-type=2": self.page(["a/1", "a/2 b"], token="t+1"),
                 "ulw-soak-x?list-type=2&continuation-token=t%2B1": self.page(["c"])}
        calls = []
        stack.s3_body = lambda path: pages[path]
        stack.s3 = lambda method, path: calls.append((method, path)) or "204"
        self.assertEqual(stack.delete_bucket(), "204")
        self.assertEqual(sorted(calls[:-1]), [("DELETE", "ulw-soak-x/a/1"),
                                              ("DELETE", "ulw-soak-x/a/2%20b"),
                                              ("DELETE", "ulw-soak-x/c")])
        self.assertEqual(calls[-1], ("DELETE", "ulw-soak-x"))


class Cleanup(unittest.TestCase):
    class FakeStack:
        def __init__(self, stop_raises=False):
            self.calls = []
            self.stop_raises = stop_raises

        def stop(self):
            self.calls.append("stop")
            if self.stop_raises:
                raise OSError("stop failed")

        def cleanup(self):
            self.calls.append("cleanup")

    def test_a_stage_that_raises_still_stops_the_services_and_cleans_up(self):
        for error in [RuntimeError("gateway never became ready"), KeyboardInterrupt()]:
            stack = self.FakeStack()
            stop = soak.threading.Event()

            def run():
                raise error

            with self.assertRaises(type(error)):
                soak.with_stack(stack, stop, run)
            self.assertTrue(stop.is_set())
            self.assertEqual(stack.calls, ["stop", "cleanup"])

    def test_cleanup_runs_even_when_stopping_fails(self):
        stack = self.FakeStack(stop_raises=True)
        with self.assertRaises(OSError):
            soak.with_stack(stack, soak.threading.Event(), lambda: None)
        self.assertEqual(stack.calls, ["stop", "cleanup"])


class Polling(unittest.TestCase):
    class FakeClient:
        def __init__(self, answers):
            self.answers = answers

        def http(self, method, path, user):
            return self.answers[path.rsplit("/", 1)[1]]

    def test_only_a_failed_state_counts_as_a_failed_video(self):
        counts = soak.Counts()
        ready = soak.ReadyVideos(counts)
        for video in ["ok", "bad", "busy", "unavailable", "dropped"]:
            ready.add("alice", video)
        ready.poll(self.FakeClient({
            "ok": (200, {}, b'{"state":"ready"}'),
            "bad": (200, {}, b'{"state":"failed","error_reason":"the file could not be decoded"}'),
            "busy": (200, {}, b'{"state":"processing"}'),
            "unavailable": (503, {}, b""),
            "dropped": (0, {}, b"")}))
        totals = counts.snapshot()
        self.assertEqual(totals.get("videos_ready_total"), 1)
        self.assertEqual(totals.get("videos_failed"), 1)
        self.assertEqual(totals.get("video_failed: the file could not be decoded"), 1)
        self.assertEqual(sorted(v for _, v in ready.pending), ["busy", "dropped", "unavailable"])
        self.assertEqual(ready.done, [("alice", "ok")])

    def test_the_preflight_clip_is_played_but_not_counted_as_made_by_the_load(self):
        counts = soak.Counts()
        ready = soak.ReadyVideos(counts)
        ready.promote("alice", "preflight")
        self.assertEqual(ready.done, [("alice", "preflight")])
        self.assertNotIn("videos_ready_total", counts.snapshot())


@unittest.skipUnless(os.geteuid() == 0, "the sandbox's view is root's without capabilities")
class ScratchReach(unittest.TestCase):
    def test_a_directory_closed_to_others_is_out_of_reach(self):
        with tempfile.TemporaryDirectory() as d:
            closed = pathlib.Path(d) / "home"
            inner = closed / "runner" / "_work"
            inner.mkdir(parents=True)
            self.assertTrue(soak.traversable_without_privilege(inner))
            # Ubuntu's home directories: 750, another user and group.
            os.chown(closed, 65534, 65534)
            os.chmod(closed, stat.S_IRWXU | stat.S_IRGRP | stat.S_IXGRP)
            self.assertFalse(soak.traversable_without_privilege(inner))
            os.chmod(closed, stat.S_IRWXU | stat.S_IRGRP | stat.S_IXGRP | stat.S_IXOTH)
            self.assertTrue(soak.traversable_without_privilege(inner))


if __name__ == "__main__":
    unittest.main()
