#!/usr/bin/env python3
"""Usage: split-resources.py OUTDIR [FILE]

Writes each Kubernetes resource in FILE (or stdin) to OUTDIR/<kind>-<namespace>-<name>.yaml,
lower-case, "cluster" for a cluster-scoped one. Trivy's ignore file scopes an entry by path
only, not by resource, so trivy-config.sh scans one resource per file and an entry in
tools/security/trivyignore.yaml can name exactly the resource it waives.
"""
import os
import sys

import yaml


def main() -> int:
    out = sys.argv[1]
    source = open(sys.argv[2], encoding="utf-8") if len(sys.argv) > 2 else sys.stdin
    os.makedirs(out, exist_ok=True)
    with source:
        for doc in yaml.safe_load_all(source):
            if not doc:
                continue
            meta = doc["metadata"]
            name = "-".join([doc["kind"], meta.get("namespace", "cluster"), meta["name"]]).lower()
            with open(f"{out}/{name}.yaml", "x", encoding="utf-8") as f:
                yaml.safe_dump(doc, f, sort_keys=False)
    return 0


if __name__ == "__main__":
    sys.exit(main())
