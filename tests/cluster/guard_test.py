#!/usr/bin/env python3
"""vod_flow.py must never reach a cluster other than the sandbox: it refuses before running
kubectl at all, and when it does run kubectl it names the sandbox's kubeconfig and context
whatever the caller's KUBECONFIG says."""
import contextlib
import io
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import vod_flow  # noqa: E402

# An address that resolves nowhere (RFC 6761): a guard that let a call through would hang or
# fail on DNS, never reach a machine.
UNROUTABLE = "https://k8s-prod.askedin.invalid:6443"


def kubeconfig(directory, server, context="kind-ulw-e2e"):
    path = pathlib.Path(directory, "kubeconfig")
    path.write_text(f"""apiVersion: v1
kind: Config
clusters:
- name: target
  cluster:
    server: {server}
contexts:
- name: {context}
  context:
    cluster: target
    user: someone
current-context: {context}
users:
- name: someone
  user:
    token: not-a-real-token
""", encoding="utf-8")
    return path


class GuardTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.dir = tmp.name
        self.calls = []

        def record(args, **_):
            self.calls.append(args)
            # Stops the scenario at its first kubectl call.
            return subprocess.CompletedProcess(args, 1, "", "recorded")

        patcher = mock.patch.object(vod_flow.subprocess, "run", side_effect=record)
        patcher.start()
        self.addCleanup(patcher.stop)
        # main() sets these; each test starts from the module's own values.
        for name in ("BASE", "TOKEN", "KUBECTL_ALLOWED"):
            state = mock.patch.object(vod_flow, name, getattr(vod_flow, name))
            state.start()
            self.addCleanup(state.stop)
        env = mock.patch.dict(os.environ, {}, clear=False)
        env.start()
        self.addCleanup(env.stop)
        for name in ("ULW_E2E_URL", "ULW_E2E_TOKEN"):
            os.environ.pop(name, None)

    def run_main(self, sandbox_kubeconfig, *argv):
        with mock.patch.object(vod_flow, "SANDBOX_KUBECONFIG", sandbox_kubeconfig), \
                contextlib.redirect_stdout(io.StringIO()), \
                self.assertRaises(SystemExit) as exit_:
            vod_flow.main(list(argv))
        return str(exit_.exception.code)

    def test_a_sandbox_kubeconfig_pointing_elsewhere_is_refused_before_any_kubectl_call(self):
        fake = kubeconfig(self.dir, UNROUTABLE)
        os.environ["KUBECONFIG"] = str(fake)
        for scenario in ("netpol", "pod-kill"):
            message = self.run_main(fake, scenario)
            self.assertIn("refusing", message)
            self.assertIn("k8s-prod.askedin.invalid", message)
        self.assertEqual(self.calls, [])

    def test_a_missing_sandbox_kubeconfig_is_refused_even_with_one_in_the_environment(self):
        os.environ["KUBECONFIG"] = str(kubeconfig(self.dir, UNROUTABLE))
        message = self.run_main(pathlib.Path(self.dir, "absent"), "netpol")
        self.assertIn("refusing", message)
        self.assertEqual(self.calls, [])

    def test_a_kubeconfig_without_the_sandbox_context_is_refused(self):
        other = kubeconfig(self.dir, "https://127.0.0.1:6443", context="stage")
        self.assertIn("refusing", self.run_main(other, "netpol"))
        self.assertEqual(self.calls, [])

    def test_kubectl_names_the_sandbox_kubeconfig_and_context_not_the_callers(self):
        sandbox = kubeconfig(self.dir, "https://127.0.0.1:41234")
        os.environ["KUBECONFIG"] = str(kubeconfig(tempfile.mkdtemp(dir=self.dir), UNROUTABLE))
        self.run_main(sandbox, "netpol")
        self.assertTrue(self.calls)
        for args in self.calls:
            self.assertEqual(args[1:5], ["--kubeconfig", str(sandbox), "--context", "kind-ulw-e2e"])

    def test_a_real_target_runs_only_the_http_scenarios_and_only_when_named(self):
        os.environ["ULW_E2E_URL"] = "https://stage.askedin.invalid"
        os.environ["ULW_E2E_TOKEN"] = "not-a-real-token"
        sandbox = kubeconfig(self.dir, "https://127.0.0.1:41234")
        for argv in ([], ["pod-kill"], ["netpol"], ["auth"], ["upload", "pod-kill"]):
            self.assertIn("refusing", self.run_main(sandbox, *argv), argv)
        self.assertEqual(self.calls, [])
        names, url, token = vod_flow.plan(["upload", "playback"], os.environ)
        self.assertEqual((names, url, token),
                         (["upload", "playback"], "https://stage.askedin.invalid",
                          "not-a-real-token"))

    def test_kubectl_refuses_when_the_target_is_not_the_sandbox(self):
        with self.assertRaises(vod_flow.Refused):
            vod_flow.kubectl("get", "pods")
        self.assertEqual(self.calls, [])


if __name__ == "__main__":
    unittest.main()
