#!/usr/bin/env python3
"""Checks components/external-turn as an overlay renders it (docs/adr/0109).

    check-external-turn.py BASE_LIVEKIT_DEPLOYMENT SELF_HOSTED.yaml... --external EXTERNAL.yaml...

For each external render: STUNner's Gateway and UDPRoute are gone; chat and the gateway are in
external mode; LiveKit's configuration is base/livekit's with only `rtc` changed, and names no
relay; every $(NAME) LiveKit's container uses is defined before it, since Kubernetes expands
only those; and the media Service admits exactly the sources its NetworkPolicy does. For each
self-hosted render: nothing of the external mode is there.
"""
import re
import sys
from pathlib import Path

import yaml

# tools/pathguard.py, which keeps each path given on the command line inside the repository
# and the temporary directories.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent / "tools"))
from pathguard import inside  # noqa: E402

REFERENCE = re.compile(r"\$\(([A-Z_][A-Z0-9_]*)\)")


def load(path):
    with open(path, encoding="utf-8") as f:
        return [doc for doc in yaml.safe_load_all(f) if doc]


def find(docs, kind, name):
    return next((d for d in docs if d["kind"] == kind and d["metadata"]["name"] == name), None)


def container(deployment, name):
    return next(c for c in deployment["spec"]["template"]["spec"]["containers"]
                if c["name"] == name)


def env_value(c, name):
    return next((e.get("value") for e in c.get("env", []) if e["name"] == name), None)


def livekit_config(c):
    return yaml.safe_load(env_value(c, "LIVEKIT_CONFIG"))


def check_external(path, base, errors):
    docs = load(path)
    for kind, name in (("Gateway", "stunner"), ("UDPRoute", "livekit")):
        if find(docs, kind, name):
            errors.append(f"{path}: {kind} {name} is still rendered")
    for deployment, name in (("chat", "chat"), ("video-gateway", "gateway")):
        c = container(find(docs, "Deployment", deployment), name)
        if env_value(c, "ULW_TURN_MODE") != "external":
            errors.append(f"{path}: {deployment} is not in external TURN mode")
    livekit = container(find(docs, "Deployment", "livekit"), "livekit")
    config = livekit_config(livekit)
    if "turn_servers" in config.get("rtc", {}):
        errors.append(f"{path}: LiveKit still names a relay")
    rest = {k: v for k, v in config.items() if k != "rtc"}
    base_rest = {k: v for k, v in base.items() if k != "rtc"}
    if rest != base_rest:
        errors.append(f"{path}: LiveKit's configuration differs from base/livekit's outside rtc")
    defined = set()
    for e in livekit.get("env", []):
        for ref in REFERENCE.findall(e.get("value") or ""):
            if ref not in defined:
                errors.append(f"{path}: LiveKit's {e['name']} uses $({ref}) before it is defined")
        defined.add(e["name"])
    for arg in livekit.get("args", []):
        for ref in REFERENCE.findall(arg):
            if ref not in defined:
                errors.append(f"{path}: LiveKit's args use $({ref}), which is not defined")
    service = find(docs, "Service", "livekit-media")
    policy = find(docs, "NetworkPolicy", "livekit-media")
    ranges = sorted(service["spec"].get("loadBalancerSourceRanges", []))
    blocks = sorted(peer["ipBlock"]["cidr"] for rule in policy["spec"]["ingress"]
                    for peer in rule["from"] if "ipBlock" in peer)
    if not ranges or ranges != blocks:
        errors.append(f"{path}: the media Service and its NetworkPolicy admit different sources")
    port = service["spec"]["ports"][0]["port"]
    advertised = env_value(livekit, "MEDIA_UDP_PORT")
    listening = next(p["containerPort"] for p in livekit["ports"] if p["name"] == "rtc-udp")
    if not str(port) == str(advertised) == str(listening):
        errors.append(f"{path}: the media port is {port} on the node, {listening} in the pod "
                      f"and {advertised} in LiveKit's candidates")


def check_self_hosted(path, errors):
    docs = load(path)
    if find(docs, "Service", "livekit-media"):
        errors.append(f"{path}: the external mode's media Service is rendered")
    c = container(find(docs, "Deployment", "chat"), "chat")
    if env_value(c, "ULW_TURN_MODE") is not None:
        errors.append(f"{path}: chat has ULW_TURN_MODE without components/external-turn")
    if not find(docs, "Gateway", "stunner"):
        errors.append(f"{path}: STUNner's Gateway is missing")


def main(argv):
    if "--external" not in argv:
        sys.exit(__doc__)
    split = argv.index("--external")
    if split == 0:
        sys.exit(__doc__)
    paths = [inside(p) for p in argv[:split]]
    base_path, self_hosted = paths[0], paths[1:]
    external = [inside(p) for p in argv[split + 1:]]
    base = livekit_config(container(load(base_path)[0], "livekit"))
    errors = []
    for path in self_hosted:
        check_self_hosted(path, errors)
    for path in external:
        check_external(path, base, errors)
    for error in errors:
        print(f"check-external-turn: {error}", file=sys.stderr)
    sys.exit(1 if errors else 0)


if __name__ == "__main__":
    main(sys.argv[1:])
