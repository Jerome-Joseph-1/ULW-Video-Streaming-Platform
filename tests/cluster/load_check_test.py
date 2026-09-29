#!/usr/bin/env python3
"""load_check.py refuses anything but the kind sandbox before it runs kubectl or sends a request,
reads limits and top output correctly, and reports a sample over a limit."""
import contextlib
import csv
import http.client
import io
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import guard_test  # noqa: E402
import load_check  # noqa: E402
import vod_flow  # noqa: E402

GATEWAY = "video-gateway-6d4f9c7b58-x2k7q"
DEPLOYMENTS = {"items": [
    {"metadata": {"name": "video-gateway"}, "spec": {"template": {"spec": {
        "initContainers": [{"name": "migrate", "resources": {"limits": {"memory": "128Mi"}}}],
        "containers": [{"name": "gateway",
                        "resources": {"limits": {"cpu": "2", "memory": "700Mi"}}}]}}}},
    {"metadata": {"name": "video-worker"}, "spec": {"template": {"spec": {
        "containers": [{"name": "worker", "resources": {"limits": {"cpu": "500m"}}}]}}}},
]}


def sample(t, pod, container, cpu_m, memory_mi):
    return {"t": t, "uploads_in_flight": None, "containers": [
        {"pod": pod, "container": container, "cpu_m": cpu_m, "memory": memory_mi << 20}]}


class RefusalTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.dir = tmp.name
        self.calls = []
        self.connections = []

        def record(args, **_):
            self.calls.append(args)
            # Stops the run at its first kubectl call.
            return subprocess.CompletedProcess(args, 1, "", "recorded")

        def refuse_connection(self_, host, *a, **k):
            self.connections.append(host)
            raise OSError("no network in tests")

        for target, patched in ((load_check.subprocess, "run"), (load_check.subprocess, "Popen")):
            patcher = mock.patch.object(target, patched, side_effect=record)
            patcher.start()
            self.addCleanup(patcher.stop)
        patcher = mock.patch.object(http.client.HTTPConnection, "__init__", refuse_connection)
        patcher.start()
        self.addCleanup(patcher.stop)
        patcher = mock.patch.object(http.client.HTTPSConnection, "__init__", refuse_connection)
        patcher.start()
        self.addCleanup(patcher.stop)
        state = mock.patch.object(vod_flow, "KUBECTL_ALLOWED", vod_flow.KUBECTL_ALLOWED)
        state.start()
        self.addCleanup(state.stop)
        env = mock.patch.dict(os.environ, {}, clear=False)
        env.start()
        self.addCleanup(env.stop)

    def run_main(self, sandbox_kubeconfig):
        with mock.patch.object(vod_flow, "SANDBOX_KUBECONFIG", sandbox_kubeconfig), \
                contextlib.redirect_stdout(io.StringIO()), \
                self.assertRaises(SystemExit) as exit_:
            load_check.main(["--out", self.dir])
        return str(exit_.exception.code)

    def test_a_sandbox_kubeconfig_pointing_elsewhere_is_refused_before_anything_runs(self):
        fake = guard_test.kubeconfig(self.dir, guard_test.UNROUTABLE)
        os.environ["KUBECONFIG"] = str(fake)
        message = self.run_main(fake)
        self.assertIn("refusing", message)
        self.assertIn("k8s-prod.askedin.invalid", message)
        self.assertEqual(self.calls, [])
        self.assertEqual(self.connections, [])

    def test_a_real_target_in_the_environment_is_ignored_and_only_the_sandbox_is_addressed(self):
        os.environ["ULW_E2E_URL"] = "https://stage.askedin.invalid"
        os.environ["ULW_E2E_TOKEN"] = "not-a-real-token"
        sandbox = guard_test.kubeconfig(self.dir, "https://127.0.0.1:41234")
        message = self.run_main(sandbox)
        self.assertIn("recorded", message)
        self.assertTrue(self.calls)
        for args in self.calls:
            self.assertEqual(args[1:5], ["--kubeconfig", str(sandbox), "--context", "kind-ulw-e2e"])
        self.assertNotIn("stage.askedin.invalid", " ".join(" ".join(c) for c in self.calls))
        self.assertEqual(self.connections, [])

    def test_a_missing_sandbox_kubeconfig_is_refused(self):
        self.assertIn("refusing", self.run_main(pathlib.Path(self.dir, "absent")))
        self.assertEqual((self.calls, self.connections), ([], []))


class QuantityTest(unittest.TestCase):
    def test_cpu_and_memory_quantities_convert_to_millicores_and_bytes(self):
        self.assertEqual(load_check.cpu_millicores("2"), 2000)
        self.assertEqual(load_check.cpu_millicores("500m"), 500)
        self.assertEqual(load_check.memory_bytes("700Mi"), 700 << 20)
        self.assertEqual(load_check.memory_bytes("2Gi"), 2 << 30)
        self.assertEqual(load_check.memory_bytes("128M"), 128_000_000)
        self.assertEqual(load_check.memory_bytes("100Ki"), 100 << 10)
        self.assertEqual(load_check.memory_bytes("1.5Gi"), 3 << 29)
        self.assertEqual(load_check.cpu_millicores("0.5"), 500)
        self.assertEqual(load_check.cpu_millicores("1500m"), 1500)
        with self.assertRaises(load_check.Failure):
            load_check.memory_bytes("lots")

    def test_limits_come_from_containers_not_init_containers(self):
        limits = load_check.limits_of(DEPLOYMENTS)
        self.assertEqual(limits[("video-gateway", "gateway")], {"cpu_m": 2000, "memory": 700 << 20})
        self.assertEqual(limits[("video-worker", "worker")], {"cpu_m": 500, "memory": None})
        self.assertNotIn(("video-gateway", "migrate"), limits)

    def test_top_output_parses_per_container_rows(self):
        rows = load_check.parse_top(f"{GATEWAY}   gateway   312m   211Mi\n")
        self.assertEqual(rows, [{"pod": GATEWAY, "container": "gateway",
                                 "cpu_m": 312, "memory": 211 << 20}])


class ViolationTest(unittest.TestCase):
    limits = load_check.limits_of(DEPLOYMENTS)

    def test_samples_at_or_under_the_limits_pass(self):
        samples = [sample(0, GATEWAY, "gateway", 2000, 700)]
        self.assertEqual(load_check.violations(samples, self.limits), [])

    def test_a_memory_sample_over_the_limit_is_named_with_pod_and_time(self):
        found = load_check.violations([sample(30, GATEWAY, "gateway", 100, 701)], self.limits)
        self.assertEqual(len(found), 1)
        self.assertIn(GATEWAY, found[0])
        self.assertIn("memory 701 MiB > limit 700 MiB", found[0])

    def test_a_cpu_sample_over_the_limit_is_reported_and_an_absent_memory_limit_is_not(self):
        worker = "video-worker-5c8d7f6b9-abcde"
        found = load_check.violations([sample(10, worker, "worker", 501, 9999)], self.limits)
        self.assertEqual(len(found), 1)
        self.assertIn("cpu 501m > limit 500m", found[0])


class RestartTest(unittest.TestCase):
    pod = ("video-gateway-abc-1", "gateway")

    def test_an_unchanged_restart_count_passes_even_with_an_oom_left_from_before_the_run(self):
        before = {self.pod: (2, "OOMKilled")}
        self.assertEqual(load_check.restart_problems(before, dict(before)), [])

    def test_a_rise_in_the_restart_count_is_reported_with_its_last_exit(self):
        found = load_check.restart_problems({self.pod: (0, None)}, {self.pod: (1, "OOMKilled")})
        self.assertEqual(len(found), 1)
        self.assertIn("0 -> 1", found[0])
        self.assertIn("OOMKilled", found[0])

    def test_a_pod_replaced_during_the_run_is_reported_though_the_new_one_shows_no_restarts(self):
        replacement = ("video-gateway-abc-2", "gateway")
        found = load_check.restart_problems({self.pod: (0, None)}, {replacement: (0, None)})
        self.assertEqual(len(found), 1)
        self.assertIn("video-gateway-abc-1", found[0])
        self.assertIn("replaced", found[0])


class PeakTest(unittest.TestCase):
    def test_a_peak_at_the_ninety_percent_line_passes(self):
        self.assertEqual(load_check.peak_problems(404, 448), [])

    def test_a_peak_just_under_ninety_percent_fails(self):
        self.assertEqual(len(load_check.peak_problems(403, 448)), 1)

    def test_no_gauge_ever_read_voids_the_check_instead_of_passing_it(self):
        found = load_check.peak_problems(None, 448)
        self.assertEqual(len(found), 1)
        self.assertIn("void", found[0])


class CoverageTest(unittest.TestCase):
    def test_a_workload_with_no_sample_is_named(self):
        samples = [sample(0, GATEWAY, "gateway", 10, 10)]
        self.assertEqual(load_check.coverage_problems(samples), ["no sample of a video-worker pod"])

    def test_samples_of_both_workloads_pass(self):
        samples = [sample(0, GATEWAY, "gateway", 10, 10),
                   sample(0, "video-worker-5c8d7f6b9-abcde", "worker", 10, 10)]
        self.assertEqual(load_check.coverage_problems(samples), [])


class ReportTest(unittest.TestCase):
    def test_reports_carry_the_samples_and_the_verdict_even_for_a_failed_run(self):
        with tempfile.TemporaryDirectory() as tmp:
            samples = [sample(10.04, GATEWAY, "gateway", 312, 211)]
            samples[0]["uploads_in_flight"] = 440
            report = {"passed": False, "violations": ["upload_load.py exited 1"]}
            load_check.write_reports(pathlib.Path(tmp, "out"), report, samples)
            written = json.loads(pathlib.Path(tmp, "out/report.json").read_text())
            self.assertEqual(written, report)
            with open(pathlib.Path(tmp, "out/samples.csv"), newline="") as f:
                rows = list(csv.reader(f))
            self.assertEqual(rows[0][0], "t_s")
            self.assertEqual(rows[1], ["10.0", GATEWAY, "gateway", "312", str(211 << 20), "440"])


class FailedRunTest(unittest.TestCase):
    def test_a_driver_that_exits_early_still_leaves_a_report_with_the_samples_taken(self):
        pods = {"items": [{"metadata": {"name": GATEWAY}, "status": {"containerStatuses": [
            {"name": "gateway", "restartCount": 0}]}}]}

        def kubectl(*args, check_rc=True):
            text = " ".join(args)
            out = ""
            if "get deployments" in text:
                out = json.dumps(DEPLOYMENTS)
            elif "top pods" in text:
                out = f"{GATEWAY} gateway 100m 50Mi\n"
            elif "get pods -o json" in text:
                out = json.dumps(pods)
            elif "get pods -l" in text:
                out = GATEWAY + "\n"
            elif "--raw" in text:
                out = "connections_current 9\nuploads_in_flight 8\n"
            return subprocess.CompletedProcess(args, 0, out, "")

        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp)
        with mock.patch.object(vod_flow, "require_sandbox"), \
                mock.patch.object(vod_flow, "kubectl", kubectl), \
                mock.patch.object(vod_flow, "mint", lambda subject: "token"), \
                mock.patch.object(vod_flow, "make_clip", lambda path, s, b: b"clip"), \
                mock.patch.object(load_check.subprocess, "run", return_value=subprocess.CompletedProcess(
                    [], 3, "", "boom")), \
                mock.patch.object(vod_flow, "KUBECTL_ALLOWED", False), \
                contextlib.redirect_stdout(io.StringIO()), \
                self.assertRaises(SystemExit) as exit_:
            load_check.main(["--out", tmp, "--uploads", "4"])
            self.fail("unreachable")
        report = json.loads(pathlib.Path(tmp, "report.json").read_text())
        self.assertFalse(report["passed"])
        self.assertTrue(any("upload_load.py exited 3" in v for v in report["violations"]))
        self.assertGreaterEqual(report["samples"], 1)
        self.assertIn("boom", str(exit_.exception.code))


if __name__ == "__main__":
    unittest.main()
