#!/usr/bin/env python3
"""load_check.py refuses anything but the kind sandbox before it runs kubectl or sends a request,
reads limits and top output correctly, and reports a sample over a limit."""
import contextlib
import io
import os
import pathlib
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
        for target in ("run", "Popen"):
            patcher = mock.patch.object(load_check.subprocess, target,
                                        side_effect=lambda *a, **k: self.calls.append(a))
            patcher.start()
            self.addCleanup(patcher.stop)
        for name in ("KUBECTL_ALLOWED",):
            state = mock.patch.object(vod_flow, name, getattr(vod_flow, name))
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

    def test_a_real_target_in_the_environment_changes_nothing(self):
        os.environ["ULW_E2E_URL"] = "https://stage.askedin.invalid"
        os.environ["ULW_E2E_TOKEN"] = "not-a-real-token"
        self.assertIn("refusing", self.run_main(pathlib.Path(self.dir, "absent")))
        self.assertEqual(self.calls, [])


class QuantityTest(unittest.TestCase):
    def test_cpu_and_memory_quantities_convert_to_millicores_and_bytes(self):
        self.assertEqual(load_check.cpu_millicores("2"), 2000)
        self.assertEqual(load_check.cpu_millicores("500m"), 500)
        self.assertEqual(load_check.memory_bytes("700Mi"), 700 << 20)
        self.assertEqual(load_check.memory_bytes("2Gi"), 2 << 30)
        self.assertEqual(load_check.memory_bytes("128M"), 128_000_000)
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


if __name__ == "__main__":
    unittest.main()
