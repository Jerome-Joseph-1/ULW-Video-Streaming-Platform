#!/usr/bin/env python3
"""check-external-turn.py passes a correct render of components/external-turn and of the
self-hosted overlays, and names each way a render can be wrong (docs/adr/0109)."""
import copy
import importlib.util
import io
import pathlib
import tempfile
import unittest
from contextlib import redirect_stderr

import yaml

HERE = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("check_external_turn",
                                              HERE / "check-external-turn.py")
check = importlib.util.module_from_spec(spec)
spec.loader.exec_module(check)

CONFIG = {"port": 7880, "rtc": {"udp_port": 7882}, "room": {"auto_create": False}}
RANGES = ["198.51.100.0/24", "203.0.113.0/24"]


def deployment(name, container, env, args=None, ports=None):
    c = {"name": container, "env": env}
    if args is not None:
        c["args"] = args
    if ports is not None:
        c["ports"] = ports
    return {"kind": "Deployment", "metadata": {"name": name},
            "spec": {"template": {"spec": {"containers": [c]}}}}


def livekit(config, env_before=(), port=7882):
    env = [{"name": n, "value": "x"} for n in env_before]
    env += [{"name": "MEDIA_PUBLIC_IP", "value": "203.0.113.10"},
            {"name": "MEDIA_UDP_PORT", "value": str(port)},
            {"name": "LIVEKIT_CONFIG", "value": yaml.safe_dump(config)}]
    return deployment("livekit", "livekit", env, args=["--node-ip=$(MEDIA_PUBLIC_IP)"],
                      ports=[{"name": "rtc-udp", "containerPort": port}])


def external_render():
    config = copy.deepcopy(CONFIG)
    config["rtc"] = {"udp_port": "$(MEDIA_UDP_PORT)", "advertise_internal_ip": True}
    turn = [{"name": "ULW_TURN_MODE", "value": "external"}]
    return [
        deployment("chat", "chat", turn),
        deployment("video-gateway", "gateway", turn),
        livekit(config),
        {"kind": "Service", "metadata": {"name": "livekit-media"},
         "spec": {"loadBalancerSourceRanges": list(RANGES), "ports": [{"port": 7882}]}},
        {"kind": "NetworkPolicy", "metadata": {"name": "livekit-media"},
         "spec": {"ingress": [{"from": [{"ipBlock": {"cidr": c}} for c in RANGES]}]}},
    ]


def self_hosted_render():
    return [deployment("chat", "chat", []),
            {"kind": "Gateway", "metadata": {"name": "stunner"}}]


class CheckExternalTurnTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.dir.name)
        config = copy.deepcopy(CONFIG)
        config["rtc"]["turn_servers"] = [{"host": "$(TURN_HOST)"}]
        self.base = self.write("base.yaml", [livekit(config)])

    def tearDown(self):
        self.dir.cleanup()

    def write(self, name, docs):
        path = self.root / name
        path.write_text(yaml.safe_dump_all(docs), encoding="utf-8")
        return str(path)

    def run_check(self, self_hosted, external):
        argv = [self.base]
        argv += [self.write(f"s{i}.yaml", d) for i, d in enumerate(self_hosted)]
        argv += ["--external"]
        argv += [self.write(f"e{i}.yaml", d) for i, d in enumerate(external)]
        err = io.StringIO()
        with redirect_stderr(err), self.assertRaises(SystemExit) as raised:
            check.main(argv)
        return raised.exception.code, err.getvalue()

    def test_correct_renders_pass(self):
        code, err = self.run_check([self_hosted_render()], [external_render()])
        self.assertEqual(code, 0, err)

    def expect(self, docs, message, self_hosted=None):
        code, err = self.run_check([self_hosted or self_hosted_render()], [docs])
        self.assertEqual(code, 1)
        self.assertIn(message, err)

    def test_stunner_still_rendered(self):
        docs = external_render() + [{"kind": "Gateway", "metadata": {"name": "stunner"}},
                                    {"kind": "UDPRoute", "metadata": {"name": "livekit"}}]
        self.expect(docs, "Gateway stunner is still rendered")

    def test_a_service_not_in_external_mode(self):
        docs = external_render()
        docs[0]["spec"]["template"]["spec"]["containers"][0]["env"] = []
        self.expect(docs, "chat is not in external TURN mode")

    def test_livekit_still_names_a_relay(self):
        docs = external_render()
        config = copy.deepcopy(CONFIG)
        config["rtc"] = {"turn_servers": []}
        docs[2] = livekit(config)
        self.expect(docs, "LiveKit still names a relay")

    def test_livekit_differs_from_the_base_outside_rtc(self):
        docs = external_render()
        config = copy.deepcopy(CONFIG)
        config["room"]["auto_create"] = True
        docs[2] = livekit(config)
        self.expect(docs, "differs from base/livekit's outside rtc")

    def test_a_variable_used_before_it_is_defined(self):
        docs = external_render()
        env = docs[2]["spec"]["template"]["spec"]["containers"][0]["env"]
        env.insert(0, env.pop())  # LIVEKIT_CONFIG first
        env[0]["value"] = "port: $(MEDIA_UDP_PORT)"
        self.expect(docs, "uses $(MEDIA_UDP_PORT) before it is defined")

    def test_args_naming_an_undefined_variable(self):
        docs = external_render()
        docs[2]["spec"]["template"]["spec"]["containers"][0]["args"] = ["--x=$(NOPE)"]
        self.expect(docs, "args use $(NOPE)")

    def test_the_two_source_lists_differ(self):
        docs = external_render()
        docs[3]["spec"]["loadBalancerSourceRanges"] = ["10.0.0.0/8"]
        self.expect(docs, "admit different sources")

    def test_the_media_port_differs(self):
        docs = external_render()
        docs[3]["spec"]["ports"][0]["port"] = 7883
        self.expect(docs, "the media port is 7883 on the node")

    def test_a_self_hosted_render_with_external_parts(self):
        docs = self_hosted_render()
        docs[0]["spec"]["template"]["spec"]["containers"][0]["env"] = [
            {"name": "ULW_TURN_MODE", "value": "external"}]
        docs[1] = {"kind": "Service", "metadata": {"name": "livekit-media"}}
        code, err = self.run_check([docs], [external_render()])
        self.assertEqual(code, 1)
        self.assertIn("chat has ULW_TURN_MODE", err)
        self.assertIn("the external mode's media Service is rendered", err)
        self.assertIn("STUNner's Gateway is missing", err)

    def test_usage(self):
        for argv in ([self.base], ["--external", self.base]):
            with self.subTest(argv=argv), self.assertRaises(SystemExit) as raised:
                check.main(argv)
            self.assertIn("check-external-turn.py", str(raised.exception.code))

    def test_a_path_outside_the_repository_and_temp_is_refused(self):
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as raised:
            check.main(["/etc/passwd", "--external", self.base])
        self.assertEqual(raised.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
