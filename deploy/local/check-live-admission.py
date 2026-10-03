#!/usr/bin/env python3
"""Checks the packagers' admission policy (deploy/askedin/overlays/<env>/live-packager/
admission-policy.yaml, docs/adr/0092) against a real API server: what the gateway's service
account may create in the packagers' namespace, and what it may not.

    deploy/local/check-live-admission.py <env> -- <kubectl command...>

The kubectl command must reach an API server (the sandbox's, or a bare kube-apiserver) as a
user who may impersonate, with the overlay's live-packager/ manifests and the gateway's
video-gateway/live-rbac.yaml applied. Every check is a server-side dry run, so nothing is
stored. The template is live-packager/job.yaml, filled in as the gateway fills it; each refused
case changes it, or the Secret, in one way the policy must refuse.
"""
import copy
import json
import subprocess
import sys
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[2]
NAMESPACES = {"stage": ("apps-stage", "apps-stage-live"), "prod": ("apps", "apps-live")}
STREAM = "0192f3a4-0000-7000-8000-0000000000aa"
# Changes the API server's own validation refuses before admission is asked, in a user
# namespace (hostUsers false) or on a server that admits no privileged pod; the policy holds
# them too, for a server that would let them through.
REFUSED_BEFORE = {"the node's network", "the node's pids", "privileged"}


def job_template(namespace: str) -> dict:
    text = (ROOT / "deploy/askedin/live-packager/job.yaml").read_text(encoding="utf-8")
    for name, value in (("ULW_NAMESPACE", namespace), ("ULW_IMAGE_TAG", "main"),
                        ("ULW_STREAM_ID", STREAM), ("ULW_STREAM_OWNER", "auth0|alice")):
        text = text.replace("${" + name + "}", value)
    return yaml.safe_load(text)


def secret(namespace: str) -> dict:
    return {"apiVersion": "v1", "kind": "Secret", "type": "Opaque",
            "metadata": {"name": f"live-packager-{STREAM}", "namespace": namespace},
            "stringData": {"ULW_LIVE_SRT_PASSPHRASE": "fake-passphrase-testtest123"}}


def pod(job: dict) -> dict:
    return job["spec"]["template"]["spec"]


def container(job: dict) -> dict:
    return pod(job)["containers"][0]


def job_cases(base: dict):
    def case(name, change):
        job = copy.deepcopy(base)
        change(job)
        return name, job

    yield case("another image", lambda j: container(j).update(image="docker.io/library/busybox"))
    yield case("a second container",
               lambda j: pod(j)["containers"].append(dict(container(j), name="second")))
    yield case("an init container",
               lambda j: pod(j).update(initContainers=[dict(container(j), name="init")]))
    yield case("a hostPath volume", lambda j: pod(j)["volumes"].append(
        {"name": "host", "hostPath": {"path": "/"}}))
    yield case("a Secret volume", lambda j: pod(j)["volumes"].append(
        {"name": "creds", "secret": {"secretName": "video-gateway-secrets"}}))
    yield case("the account's token", lambda j: pod(j).update(automountServiceAccountToken=True))
    yield case("another account", lambda j: pod(j).update(serviceAccountName="video-gateway"))
    def node_users(j):
        pod(j).update(hostUsers=True)
        container(j)["securityContext"].pop("procMount")

    yield case("the node's users", node_users)
    yield case("the node's network", lambda j: pod(j).update(hostNetwork=True))
    yield case("the node's pids", lambda j: pod(j).update(hostPID=True))
    yield case("privileged", lambda j: container(j)["securityContext"].update(privileged=True))
    yield case("privilege escalation",
               lambda j: container(j)["securityContext"].update(allowPrivilegeEscalation=True))
    yield case("a whole Secret as env", lambda j: container(j).update(
        envFrom=[{"secretRef": {"name": "live-packager-secrets"}}]))
    yield case("another stream's Secret", lambda j: container(j)["env"].append(
        {"name": "X", "valueFrom": {"secretKeyRef": {"name": "live-packager-other",
                                                     "key": "ULW_LIVE_SRT_PASSPHRASE"}}}))
    yield case("another Secret", lambda j: container(j)["env"].append(
        {"name": "X", "valueFrom": {"secretKeyRef": {"name": "video-gateway-secrets",
                                                     "key": "ULW_DATABASE_URL"}}}))
    yield case("another name", lambda j: j["metadata"].update(name="not-the-instance"))
    yield case("another kind of workload",
               lambda j: j["metadata"]["labels"].update({"app.kubernetes.io/name": "x"}))


def secret_cases(base: dict):
    other = copy.deepcopy(base)
    other["metadata"]["name"] = "video-gateway-secrets"
    yield "a Secret of another name", other
    token = copy.deepcopy(base)
    token["type"] = "kubernetes.io/service-account-token"
    token["metadata"]["annotations"] = {"kubernetes.io/service-account.name": "default"}
    yield "a Secret of another type", token


def dry_run(kubectl: list[str], gateway: str, manifest: dict) -> subprocess.CompletedProcess:
    return subprocess.run(
        [*kubectl, f"--as={gateway}", "create", "--dry-run=server", "-o", "name", "-f", "-"],
        input=json.dumps(manifest), capture_output=True, text=True, check=False)


def main() -> int:
    if len(sys.argv) < 4 or sys.argv[1] not in NAMESPACES or sys.argv[2] != "--":
        print(__doc__, file=sys.stderr)
        return 2
    kubectl = sys.argv[3:]
    gateway_ns, live = NAMESPACES[sys.argv[1]]
    gateway = f"system:serviceaccount:{gateway_ns}:video-gateway"
    failures = []
    job = job_template(live)
    for name, manifest in (("the template's Job", job), ("the stream's Secret", secret(live))):
        done = dry_run(kubectl, gateway, manifest)
        if done.returncode != 0:
            failures.append(f"{name}: refused: {done.stderr.strip()}")
        else:
            print(f"admitted: {name}")
    for name, manifest in [*job_cases(job), *secret_cases(secret(live))]:
        done = dry_run(kubectl, gateway, manifest)
        if done.returncode == 0:
            failures.append(f"{name}: admitted")
        elif "ValidatingAdmissionPolicy" not in done.stderr and name not in REFUSED_BEFORE:
            failures.append(f"{name}: refused, but not by the policy: {done.stderr.strip()}")
        else:
            print(f"refused: {name}")
    # Anyone else is not held to it: an operator's by-hand start goes through as it always did.
    unbound = copy.deepcopy(job)
    container(unbound)["image"] = "docker.io/library/busybox"
    done = subprocess.run([*kubectl, "create", "--dry-run=server", "-o", "name", "-f", "-"],
                          input=json.dumps(unbound), capture_output=True, text=True, check=False)
    if done.returncode != 0:
        failures.append(f"another user's Job: refused: {done.stderr.strip()}")
    else:
        print("admitted: another user's Job")
    for failure in failures:
        print(f"check-live-admission: {failure}", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
