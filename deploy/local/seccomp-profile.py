#!/usr/bin/env python3
"""Derives the video-worker's seccomp profile from Docker's default one.

    seccomp-profile.py MOBY_DEFAULT_JSON OUT_JSON

ulw_sandbox confines every ffmpeg in fresh user, network, mount and pid namespaces
(docs/adr/0025), which takes unshare, mount and mount_setattr. The runtime's default profile
allows those only to a process holding CAP_SYS_ADMIN, which the worker never has, so the worker
needs a profile of its own; Unconfined would drop the filter for ffmpeg as well.

Docker's profile carries conditions (a rule applies only with some capability, or on some
architecture) that only Docker evaluates: the kubelet hands a Localhost profile to the runtime
as a plain OCI seccomp profile and would ignore them, which would allow every conditional rule
unconditionally. So this evaluates them for the worker's container exactly: x86-64, no
capabilities at all (it drops ALL). Then it allows the three calls above, and nothing else
beyond the default.
"""
import json
import sys
from pathlib import Path

# tools/pathguard.py, which keeps each path given on the command line inside the repository
# and the temporary directories.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent / "tools"))
from pathguard import inside  # noqa: E402

ARCH = "SCMP_ARCH_X86_64"
# The Go architecture names Docker's conditions use for ARCH.
ARCH_NAMES = {"amd64", "x86_64"}
EXTRA = ["mount", "mount_setattr", "unshare"]


def applies(rule):
    includes = rule.get("includes", {})
    excludes = rule.get("excludes", {})
    if includes.get("caps"):
        return False
    if includes.get("arches") and not ARCH_NAMES & set(includes["arches"]):
        return False
    if ARCH_NAMES & set(excludes.get("arches", [])):
        return False
    return True


def main(source, target):
    source = inside(source)
    target = inside(target)
    with open(source, encoding="utf-8") as f:
        moby = json.load(f)
    arches = next(a for a in moby["archMap"] if a["architecture"] == ARCH)
    syscalls = []
    for rule in moby["syscalls"]:
        if not applies(rule):
            continue
        kept = {"names": rule["names"], "action": rule["action"]}
        for key in ("args", "errnoRet"):
            if rule.get(key) is not None:
                kept[key] = rule[key]
        syscalls.append(kept)
    syscalls.append({"names": EXTRA, "action": "SCMP_ACT_ALLOW"})
    profile = {
        "defaultAction": moby["defaultAction"],
        "defaultErrnoRet": moby["defaultErrnoRet"],
        "architectures": [ARCH, *arches["subArchitectures"]],
        "syscalls": syscalls,
    }
    with open(target, "w", encoding="utf-8") as f:
        json.dump(profile, f, indent=2)
        f.write("\n")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
