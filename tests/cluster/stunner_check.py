#!/usr/bin/env python3
"""M26 in the sandbox cluster (deploy/stunner/up.sh): STUNner is Ready and Programmed, and a
client outside the cluster's network gets from it what a browser on the internet would.

  1. The operator and the stunnerd and LiveKit Deployments are Available, the GatewayClass
     Accepted, the Gateway Accepted and Programmed, and the UDPRoute Accepted with its LiveKit
     backend resolved. Each wait has a deadline.
  2. From a container on the outside network (deploy/local/sandbox.sh), at an address fixed
     here, turn_probe.py sends a hand-built Binding request to 198.18.0.1:3478, which reaches
     STUNner through the host's DNAT, the node's NodePort and the Service, as a browser's
     packets reach k8s-prod. XOR-MAPPED-ADDRESS must be that container's address and port:
     anything masquerading on the way (the Service's default traffic policy, Docker's NAT)
     shows up as another address.
  3. A TURN Allocate with a credential minted from the sandbox's shared secret succeeds, signed
     with the same key, and relays from the stunnerd pod; a permission to the LiveKit pod is
     granted and one to a pod that is no backend (the operator) refused, so it is no open
     relay into the cluster; a wrong password and an expired credential are both refused.

    tests/cluster/stunner_check.py

Like vod_flow.py, kubectl only ever runs against the sandbox: deploy/local/.state/kubeconfig with
context kind-ulw-e2e, whose API server must be on 127.0.0.1, checked before anything runs.
"""
import base64
import json
import pathlib
import re
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import vod_flow  # noqa: E402
from vod_flow import Failure, Refused, check, kubectl  # noqa: E402

ROOT = vod_flow.ROOT
HERE = pathlib.Path(__file__).resolve().parent
NAMESPACE = "apps-stage"
OUTSIDE_NETWORK = "ulw-e2e-outside"
TURN_SERVER = ("198.18.0.1", 3478)
# The probe's address on the outside network, fixed so the expected mapping comes from here and
# not from the probe's own report. Far from the gateway and from Docker's first leases.
PROBE_ADDRESS = "198.18.0.200"
WAIT_TIMEOUT = "120s"


def pinned_probe_image():
    text = (ROOT / "deploy/local/images.sh").read_text(encoding="utf-8")
    match = re.search(r"^probe_image=(\S+)$", text, re.MULTILINE)
    check(match is not None, "images.sh names no probe_image")
    return match.group(1)


def conditions(namespace, resource):
    """Every condition of `resource`, its Gateway API parent statuses' included, by type."""
    scope = ["-n", namespace] if namespace else []
    status = json.loads(kubectl(*scope, "get", resource, "-o", "json").stdout).get("status", {})
    found = {c["type"]: c["status"] for c in status.get("conditions", [])}
    for parent in status.get("parents", []):
        found.update({c["type"]: c["status"] for c in parent.get("conditions", [])})
    return found


def wait_ready():
    """Step 1. Prints the conditions it saw as evidence."""
    kubectl("-n", "stunner-system", "wait", "--for=condition=Available", "--timeout", WAIT_TIMEOUT,
            "deployment/stunner-gateway-operator-controller-manager")
    kubectl("wait", "--for=condition=Accepted", "--timeout", WAIT_TIMEOUT,
            "gatewayclass/stunner-gatewayclass")
    kubectl("-n", NAMESPACE, "wait", "--for=condition=Programmed", "--timeout", WAIT_TIMEOUT,
            "gateway/stunner")
    for condition in ("Accepted", "ResolvedRefs"):
        kubectl("-n", NAMESPACE, "wait", "--timeout", WAIT_TIMEOUT,
                "--for=jsonpath={.status.parents[0].conditions[?(@.type==\"%s\")].status}=True"
                % condition, "udproutes.stunner.l7mp.io/livekit")
    kubectl("-n", NAMESPACE, "wait", "--for=create", "--timeout", WAIT_TIMEOUT,
            "deployment/stunner")
    kubectl("-n", NAMESPACE, "wait", "--for=condition=Available", "--timeout", WAIT_TIMEOUT,
            "deployment/stunner", "deployment/livekit")
    wanted = [
        (None, "gatewayclass/stunner-gatewayclass", ("Accepted",)),
        (NAMESPACE, "gateway/stunner", ("Accepted", "Programmed")),
        (NAMESPACE, "udproutes.stunner.l7mp.io/livekit", ("Accepted", "ResolvedRefs")),
        (NAMESPACE, "deployment/stunner", ("Available",)),
        (NAMESPACE, "deployment/livekit", ("Available",)),
    ]
    for namespace, resource, types in wanted:
        seen = conditions(namespace, resource)
        print(f"  {resource}: " + ", ".join(f"{t}={seen.get(t)}" for t in types))
        for t in types:
            check(seen.get(t) == "True", f"{resource}: {t} is {seen.get(t)}")


def pod_ip(namespace, selector):
    ips = kubectl("-n", namespace, "get", "pods", "-l", selector, "--field-selector",
                  "status.phase=Running", "-o", "jsonpath={.items[*].status.podIP}").stdout.split()
    check(len(ips) == 1, f"{selector} in {namespace}: running pods at {ips}, want one")
    return ips[0]


def shared_secret():
    encoded = kubectl("-n", "stunner-system", "get", "secret", "stunner-secrets", "-o",
                      "jsonpath={.data.secret}").stdout
    return base64.b64decode(encoded).decode()


def probe(livekit, forbidden, secret):
    result = subprocess.run(
        ["docker", "run", "--rm", "--network", OUTSIDE_NETWORK, "--ip", PROBE_ADDRESS,
         "--read-only", "--volume", f"{HERE}:/probe:ro", pinned_probe_image(),
         "python3", "/probe/turn_probe.py", TURN_SERVER[0], str(TURN_SERVER[1]),
         "--secret", secret, "--user", "stunner-check", "--permit", livekit,
         "--forbid", forbidden],
        capture_output=True, text=True)
    if result.returncode != 0:
        raise Failure(f"probe: {result.stderr.strip()}")
    return json.loads(result.stdout)


def verify(report, stunnerd, livekit, forbidden):
    """Steps 2 and 3 on the probe's report."""
    binding = report["binding"]
    print(f"  binding from {binding['local']}: XOR-MAPPED-ADDRESS {binding['mapped']}")
    check(binding["local"][0] == PROBE_ADDRESS,
          f"probe bound {binding['local']}, not on {PROBE_ADDRESS}")
    check(binding["mapped"] == binding["local"],
          f"XOR-MAPPED-ADDRESS {binding['mapped']} is not the client's {binding['local']}")

    allocation = report["allocate"]
    print(f"  allocate: {allocation['class']}, integrity {allocation.get('integrity')}, "
          f"mapped {allocation.get('mapped')}, relayed {allocation.get('relayed')}")
    check(allocation["class"] == "success", f"allocate with the sandbox credential: {allocation}")
    check(allocation["integrity"] is True, "allocate success not signed with the credential's key")
    check(allocation["mapped"] == allocation["local"],
          f"allocate XOR-MAPPED-ADDRESS {allocation['mapped']} is not {allocation['local']}")
    check(allocation["relayed"][0] == stunnerd,
          f"relayed address {allocation['relayed']} is not the stunnerd pod's {stunnerd}")

    permitted, refused = report["permit"][livekit], report["forbid"][forbidden]
    print(f"  permission to LiveKit {livekit}: {permitted['class']}; to the operator "
          f"{forbidden}: {refused.get('error', refused['class'])}")
    check(permitted["class"] == "success", f"permission to the LiveKit pod: {permitted}")
    # RFC 8656 section 9.2 asks for 403, but STUNner v1.2.1 (pion/turn) answers a refused
    # permission with an error response that carries no ERROR-CODE at all; the error class is
    # what a client acts on either way, and anything but a success means no relay to that peer.
    check(refused["class"] == "error" and refused["error"][0] in (0, 403),
          f"permission to a pod outside the UDPRoute: {refused}, want a refusal")
    check(report["release"]["class"] == "success", f"releasing the allocation: {report['release']}")

    # RFC 8656 has both answered 401. pion/turn, under STUNner v1.2.1, answers a request whose
    # MESSAGE-INTEGRITY does not verify with 400 instead; either way no relay is allocated.
    for case in ("wrong_password", "expired"):
        got = report[case]
        print(f"  allocate with {case.replace('_', ' ')}: {got.get('error', got['class'])}")
        check(got["class"] == "error" and got["error"][0] in (400, 401),
              f"{case}: {got}, want a 400 or 401 refusal")


def main():
    try:
        vod_flow.require_sandbox(vod_flow.SANDBOX_KUBECONFIG)
    except Refused as e:
        sys.exit(f"stunner_check: refusing: {e}")
    vod_flow.KUBECTL_ALLOWED = True
    try:
        print("ready:")
        wait_ready()
        stunnerd = pod_ip(NAMESPACE, "stunner.l7mp.io/related-gateway-name=stunner")
        livekit = pod_ip(NAMESPACE, "app.kubernetes.io/name=livekit")
        forbidden = pod_ip("stunner-system",
                           "control-plane=stunner-gateway-operator-controller-manager")
        print(f"outside ({PROBE_ADDRESS} on {OUTSIDE_NETWORK} -> "
              f"{TURN_SERVER[0]}:{TURN_SERVER[1]}):")
        verify(probe(livekit, forbidden, shared_secret()), stunnerd, livekit, forbidden)
    except Failure as e:
        sys.exit(f"FAILED: {e}")
    print("ok")


if __name__ == "__main__":
    main()
