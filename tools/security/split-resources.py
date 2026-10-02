#!/usr/bin/env python3
"""Usage: split-resources.py OUTDIR [FILE]

Writes each Kubernetes resource in FILE (or stdin) to OUTDIR/<kind>-<namespace>-<name>.yaml,
lower-case, "cluster" for a cluster-scoped one. Trivy's ignore file scopes an entry by path
only, not by resource, so trivy-config.sh scans one resource per file and an entry in
tools/security/trivyignore.yaml can name exactly the resource it waives.

The file name comes from the manifest, so it must be one plain path component: a kind,
namespace or name holding '/', or anything else Kubernetes does not allow in a name, is refused
rather than written outside OUTDIR (RBAC names may hold ':', which is allowed).
"""
import os
import re
import sys
from pathlib import Path

import yaml

# tools/pathguard.py, which keeps each path given on the command line inside the repository
# and the temporary directories.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from pathguard import inside  # noqa: E402

FILE_NAME = re.compile(r"[a-z0-9][a-z0-9.:_-]*")


def main(argv: list[str]) -> int:
    out = inside(argv[0])
    source = open(inside(argv[1]), encoding="utf-8") if len(argv) > 1 else sys.stdin
    os.makedirs(out, exist_ok=True)
    with source:
        for doc in yaml.safe_load_all(source):
            if not doc:
                continue
            meta = doc["metadata"]
            name = "-".join([doc["kind"], meta.get("namespace", "cluster"), meta["name"]]).lower()
            if not FILE_NAME.fullmatch(name):
                print(f"split-resources: {name!r} is not a file name; is the manifest well formed?",
                      file=sys.stderr)
                return 1
            with open(os.path.join(out, f"{name}.yaml"), "x", encoding="utf-8") as f:
                yaml.safe_dump(doc, f, sort_keys=False)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
