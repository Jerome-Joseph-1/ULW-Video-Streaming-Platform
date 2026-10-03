#!/usr/bin/env python3
"""Checks the packagers' admission policy (deploy/kubernetes/base/live-packager/
admission-policy.yaml, docs/adr/0092) against a real API server: what the gateway's service
account may create in the packagers' namespace, and what it may not.

    deploy/local/check-live-admission.py <overlay> -- <kubectl command...>

<overlay> names deploy/kubernetes/overlays/<overlay>, whose config.env gives the namespaces and
fills the template. The kubectl command must reach an API server (the sandbox's, or a bare
kube-apiserver) as a user who may impersonate, with that overlay's namespaces, its
live-packager part and the gateway's service account applied (RUNBOOK.md, step 9). Every check
is a server-side dry run, so nothing is stored. The template is live-packager/job.yaml, filled
in as the gateway fills it; each refused case changes it, or the Secret, in one way the policy
must refuse.
"""
import copy
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[2]
KUBE = ROOT / "deploy/kubernetes"
STREAM = "0192f3a4-0000-7000-8000-0000000000aa"
# Changes the API server's own validation refuses before admission is asked, in a user
# namespace (hostUsers false) or on a server that admits no privileged pod; the policy holds
# them too, for a server that would let them through.
REFUSED_BEFORE = {"the node's network", "the node's pids", "privileged"}


def config_env(overlay: str) -> dict[str, str]:
    """The overlay's config.env, KEY=value lines, as kustomize and a shell read it."""
    values = {}
    text = (KUBE / "overlays" / overlay / "config.env").read_text(encoding="utf-8")
    for line in text.splitlines():
        if line and not line.startswith("#"):
            key, _, value = line.partition("=")
            values[key] = value
    return values


def job_template(config: dict[str, str]) -> dict:
    text = (KUBE / "live-packager/job.yaml").read_text(encoding="utf-8")
    values = {**config, "ULW_STREAM_ID": STREAM, "ULW_STREAM_OWNER": "auth0|alice"}
    for name in ("LIVE_NAMESPACE", "LIVE_PACKAGER_IMAGE_TAG", "IMAGE_PULL_POLICY", "STORAGE",
                 "R2_ACCOUNT_ID", "S3_ENDPOINT", "BUCKET", "LIVE_PACKAGER_SECRET",
                 "ULW_STREAM_ID", "ULW_STREAM_OWNER"):
        text = text.replace("${" + name + "}", values[name])
    return yaml.safe_load(text)


def owner(name: str = STREAM, kind: str = "Job", api_version: str = "batch/v1",
          uid: str = "00000000-0000-4000-8000-000000000001") -> dict:
    return {"apiVersion": api_version, "kind": kind, "name": name, "uid": uid}


def secret(namespace: str) -> dict:
    return {"apiVersion": "v1", "kind": "Secret", "type": "Opaque",
            "metadata": {"name": f"live-packager-{STREAM}", "namespace": namespace,
                         "ownerReferences": [owner()]},
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
    yield case("the gateway's Secret", lambda j: container(j)["env"].append(
        {"name": "X", "valueFrom": {"secretKeyRef": {"name": "video-gateway-secrets",
                                                     "key": "ULW_DATABASE_URL"}}}))
    yield case("another name", lambda j: j["metadata"].update(name="not-the-instance"))

    def not_a_stream(j):
        # Named, labelled and addressed alike, only not a stream's id.
        name = "live-packager-secrets"
        j["metadata"]["name"] = name
        j["metadata"]["labels"]["app.kubernetes.io/instance"] = name
        pod(j)["hostname"] = name

    yield case("a name not a stream's", not_a_stream)
    yield case("a command", lambda j: container(j).update(command=["/bin/sh", "-c", "id"]))
    yield case("arguments", lambda j: container(j).update(args=["--help"]))
    yield case("another hostname", lambda j: pod(j).update(hostname="packager"))
    yield case("no hostname", lambda j: pod(j).pop("hostname"))
    yield case("another subdomain", lambda j: pod(j).update(subdomain="video-gateway"))
    yield case("a node", lambda j: pod(j).update(nodeName="node-1"))
    yield case("a priority class",
               lambda j: pod(j).update(priorityClassName="system-node-critical"))
    yield case("another kind of workload",
               lambda j: j["metadata"]["labels"].update({"app.kubernetes.io/name": "x"}))


def secret_cases(base: dict):
    def case(name, change):
        made = copy.deepcopy(base)
        change(made)
        return name, made

    yield case("a Secret of another name",
               lambda s: s["metadata"].update(name="video-gateway-secrets"))
    yield case("the shared Secret", lambda s: s["metadata"].update(name="live-packager-secrets"))
    yield case("a Secret named past a stream's",
               lambda s: s["metadata"].update(name=f"live-packager-{STREAM}-x"))
    yield case("a Secret of no owner", lambda s: s["metadata"].pop("ownerReferences"))
    yield case("a Secret owned by another stream's Job", lambda s: s["metadata"].update(
        ownerReferences=[owner("0192f3a4-0000-7000-8000-0000000000bb")]))
    yield case("a Secret owned by another kind", lambda s: s["metadata"].update(
        ownerReferences=[owner(kind="Deployment", api_version="apps/v1")]))
    yield case("a Secret of two owners", lambda s: s["metadata"].update(
        ownerReferences=[owner(), owner(kind="Deployment", api_version="apps/v1",
                                        uid="00000000-0000-4000-8000-000000000002")]))
    token = copy.deepcopy(base)
    token["type"] = "kubernetes.io/service-account-token"
    token["metadata"]["annotations"] = {"kubernetes.io/service-account.name": "default"}
    yield "a Secret of another type", token


# What the command line may name: an overlay directory, a kubectl binary and kubectl's own
# --flag or --flag=value options (the server, a token, a kubeconfig), nothing that kubectl would
# read as another verb or file. A namespace is a DNS label.
OVERLAY = re.compile(r"^[a-z0-9][a-z0-9-]{0,62}$")
FLAG = re.compile(r"^--[a-z][a-z0-9-]*(=[^\s]+)?$")
LABEL = re.compile(r"^[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?$")


def kubectl_command(args: list[str]) -> list[str] | None:
    """The kubectl to run, as an absolute path, and its flags; None for anything else."""
    if not args or Path(args[0]).name != "kubectl":
        return None
    binary = shutil.which(args[0])
    if binary is None or not all(FLAG.match(flag) for flag in args[1:]):
        return None
    return [str(Path(binary).resolve()), *args[1:]]


def dry_run(kubectl: list[str], gateway: str, manifest: dict) -> subprocess.CompletedProcess:
    return subprocess.run(
        [*kubectl, f"--as={gateway}", "create", "--dry-run=server", "-o", "name", "-f", "-"],
        input=json.dumps(manifest), capture_output=True, text=True, check=False)


def main() -> int:
    kubectl = kubectl_command(sys.argv[3:])
    if (len(sys.argv) < 4 or sys.argv[2] != "--" or kubectl is None
            or not OVERLAY.match(sys.argv[1])
            or not (KUBE / "overlays" / sys.argv[1] / "config.env").is_file()):
        print(__doc__, file=sys.stderr)
        return 2
    config = config_env(sys.argv[1])
    if not LABEL.match(config["NAMESPACE"]) or not LABEL.match(config["LIVE_NAMESPACE"]):
        print("check-live-admission: NAMESPACE and LIVE_NAMESPACE must be DNS labels",
              file=sys.stderr)
        return 2
    live = config["LIVE_NAMESPACE"]
    gateway = f"system:serviceaccount:{config['NAMESPACE']}:video-gateway"
    failures = []
    job = job_template(config)
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
