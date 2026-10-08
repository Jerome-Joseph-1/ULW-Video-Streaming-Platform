#!/usr/bin/env python3
"""Checks the packagers' admission policy (deploy/kubernetes/base/live-packager/
admission-policy.yaml, docs/adr/0092) against a real API server: what the gateway's service
account may create in the packagers' namespace, and what it may not.

    deploy/local/check-live-admission.py <overlay> [--context <name>]
    deploy/local/check-live-admission.py <overlay> --sandbox

<overlay> names a directory under deploy/kubernetes/overlays, whose config.env gives the
namespaces and fills the template. The API server (a cluster's, or a bare kube-apiserver) is
reached by the kubectl on PATH, with its own kubeconfig (KUBECONFIG, or ~/.kube/config) and its
current context, or the context --context names, which must be one that kubeconfig lists.
--sandbox uses the sandbox's own kubectl, kubeconfig and context instead (deploy/local/
sandbox.sh). Nothing else on the command line reaches kubectl. Its user must be one who may
impersonate, with that overlay's namespaces, its live-packager part and the gateway's service
account applied (RUNBOOK.md, step 9). Every check is a server-side dry run, so nothing is stored.
The template is live-packager/job.yaml, filled in as the gateway fills it; each refused case
changes it, or the Secret, in one way the policy must refuse.
"""
import argparse
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


def config_env(overlay: Path) -> dict[str, str]:
    """The overlay's config.env, KEY=value lines, as kustomize and a shell read it."""
    values = {}
    text = (overlay / "config.env").read_text(encoding="utf-8")
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


# A namespace is a DNS label (RFC 1123); a context's name starts with no '-' and holds no space
# or shell character (an EKS or GKE context's ':', '/', '@' and '_' are kept).
LABEL = re.compile(r"\A([a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?)\Z")
CONTEXT = re.compile(r"\A([A-Za-z0-9][A-Za-z0-9._@:/-]{0,252})\Z")
# The sandbox's kubectl, kubeconfig and context, as deploy/local/sandbox.sh and tools.sh name
# them.
SANDBOX_KUBECTL = ROOT / "deploy/local/.tools/kubectl"
SANDBOX_KUBECONFIG = ROOT / "deploy/local/.state/kubeconfig"
SANDBOX_CONTEXT = "kind-ulw-e2e"


class Refused(Exception):
    """A command line or an overlay this script will not run with."""


def overlay_dir(name: str) -> Path:
    """The overlay directory whose entry is named exactly name, taken from the directory's own
    listing, so the path is never built from the command line's string."""
    for entry in sorted((KUBE / "overlays").iterdir()):
        if entry.name == name and (entry / "config.env").is_file():
            return entry
    raise Refused(f"no overlay of that name under {KUBE / 'overlays'}")


def dns_label(config: dict[str, str], key: str) -> str:
    match = LABEL.match(config.get(key, ""))
    if match is None:
        raise Refused("NAMESPACE and LIVE_NAMESPACE must be DNS labels")
    return match.group(1)


def kubectl_on_path() -> str:
    found = shutil.which("kubectl")
    if found is None:
        raise Refused("no kubectl on PATH")
    return str(Path(found).resolve())


def contexts(kubectl: str) -> list[str]:
    """The contexts kubectl's own kubeconfig lists."""
    done = subprocess.run([kubectl, "config", "get-contexts", "-o", "name"],
                          capture_output=True, text=True, check=False)
    if done.returncode != 0:
        raise Refused(f"kubectl config get-contexts: {done.stderr.strip()}")
    return [line for line in done.stdout.splitlines() if CONTEXT.match(line)]


def kubectl_command(sandbox: bool, context: str | None) -> list[str]:
    """kubectl and its fixed options. No value is the command line's own: the program is the
    one on PATH (or the sandbox's), and a context is the entry of kubectl's own listing that
    matches the name asked for."""
    if sandbox:
        if context is not None:
            raise Refused("--sandbox names its own context")
        if not SANDBOX_KUBECTL.is_file() or not SANDBOX_KUBECONFIG.is_file():
            raise Refused("the sandbox's kubectl or kubeconfig is missing; run make e2e-up first")
        return [str(SANDBOX_KUBECTL), "--kubeconfig", str(SANDBOX_KUBECONFIG),
                "--context", SANDBOX_CONTEXT]
    kubectl = kubectl_on_path()
    if context is None:
        return [kubectl]
    if CONTEXT.match(context) is None:
        raise Refused("a context is a name kubectl's kubeconfig lists")
    for known in contexts(kubectl):
        if known == context:
            return [kubectl, "--context", known]
    raise Refused("kubectl's kubeconfig lists no context of that name")


def parse(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        prog="check-live-admission.py", description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter, allow_abbrev=False)
    parser.add_argument("overlay")
    where = parser.add_mutually_exclusive_group()
    where.add_argument("--context")
    where.add_argument("--sandbox", action="store_true")
    return parser.parse_args(argv)


def dry_run(kubectl: list[str], manifest: dict,
            gateway: str | None = None) -> subprocess.CompletedProcess:
    impersonate = [f"--as={gateway}"] if gateway else []
    return subprocess.run(
        [*kubectl, *impersonate, "create", "--dry-run=server", "-o", "name", "-f", "-"],
        input=json.dumps(manifest), capture_output=True, text=True, check=False)


def main(argv: list[str] | None = None) -> int:
    args = parse(sys.argv[1:] if argv is None else argv)
    try:
        overlay = overlay_dir(args.overlay)
        config = config_env(overlay)
        namespace = dns_label(config, "NAMESPACE")
        live = dns_label(config, "LIVE_NAMESPACE")
        kubectl = kubectl_command(args.sandbox, args.context)
    except Refused as refused:
        print(f"check-live-admission: {refused}", file=sys.stderr)
        return 2
    config = {**config, "NAMESPACE": namespace, "LIVE_NAMESPACE": live}
    gateway = f"system:serviceaccount:{namespace}:video-gateway"
    failures = []
    job = job_template(config)
    for name, manifest in (("the template's Job", job), ("the stream's Secret", secret(live))):
        done = dry_run(kubectl, manifest, gateway)
        if done.returncode != 0:
            failures.append(f"{name}: refused: {done.stderr.strip()}")
        else:
            print(f"admitted: {name}")
    for name, manifest in [*job_cases(job), *secret_cases(secret(live))]:
        done = dry_run(kubectl, manifest, gateway)
        if done.returncode == 0:
            failures.append(f"{name}: admitted")
        elif "ValidatingAdmissionPolicy" not in done.stderr and name not in REFUSED_BEFORE:
            failures.append(f"{name}: refused, but not by the policy: {done.stderr.strip()}")
        else:
            print(f"refused: {name}")
    # Anyone else is not held to it: an operator's by-hand start goes through as it always did.
    unbound = copy.deepcopy(job)
    container(unbound)["image"] = "docker.io/library/busybox"
    done = dry_run(kubectl, unbound)
    if done.returncode != 0:
        failures.append(f"another user's Job: refused: {done.stderr.strip()}")
    else:
        print("admitted: another user's Job")
    for failure in failures:
        print(f"check-live-admission: {failure}", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
