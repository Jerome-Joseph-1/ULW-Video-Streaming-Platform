#!/usr/bin/env python3
"""Writes a kubeconform JSON schema for every served version of every CRD in the given files.

    crd-schemas.py OUT_DIR CRD_FILE...

Each lands at OUT_DIR/<group>/<kind>_<version>.json (kind lowercased), the layout the
-schema-location template in validate-manifests.sh names. Objects that list their properties
are closed, as kubeconform's own strict schemas are, so a misspelt field fails validation
instead of being silently dropped by the API server.
"""
import json
import pathlib
import sys

import yaml


def strict(node):
    if isinstance(node, list):
        return [strict(n) for n in node]
    if not isinstance(node, dict):
        return node
    node = {k: strict(v) for k, v in node.items()}
    if node.pop("x-kubernetes-int-or-string", False):
        node.pop("type", None)
        node["anyOf"] = [{"type": "integer"}, {"type": "string"}]
    if (
        "properties" in node
        and "additionalProperties" not in node
        and not node.get("x-kubernetes-preserve-unknown-fields")
    ):
        node["additionalProperties"] = False
    return node


def main(out_dir, paths):
    written = 0
    for path in paths:
        with open(path, encoding="utf-8") as f:
            for doc in yaml.safe_load_all(f):
                if not doc or doc.get("kind") != "CustomResourceDefinition":
                    continue
                spec = doc["spec"]
                kind = spec["names"]["kind"].lower()
                for version in spec["versions"]:
                    if not version.get("served"):
                        continue
                    schema = strict(version["schema"]["openAPIV3Schema"])
                    target = pathlib.Path(out_dir, spec["group"], f"{kind}_{version['name']}.json")
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_text(json.dumps(schema), encoding="utf-8")
                    written += 1
    if written == 0:
        sys.exit("crd-schemas: no CustomResourceDefinition found")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2:])
