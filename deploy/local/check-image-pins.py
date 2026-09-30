#!/usr/bin/env python3
"""Every image a manifest names is pinned by digest (docs/adr/0072), which is what lets Trivy's
KSV-0125 trust a whole registry such as docker.io (tools/security/trivy-data/registries.yaml).

  check-image-pins.py --images-sh deploy/local/images.sh \\
      --askedin FILE... --host FILE... --sandbox RENDERED.yaml...

Every `image:` value in the files (Kubernetes manifests, compose, kind and Woodpecker files)
must carry @sha256:<64 hex>, with these exceptions, each only where it can occur:

  --askedin  what Askedin runs (deploy/askedin): this repository's own builds,
             git.askedin.com/askedin/askedin-monorepo/..., which Woodpecker pushes under the
             branch's name (deploy/askedin/woodpecker.yml) and the overlays follow by that name.
  --host     what the host runs directly (compose.yaml, kind.yaml): ulw/..., the sandbox's own
             builds, never pulled.
  --sandbox  the sandbox's kustomizations as rendered: ulw/... likewise; and an upstream image is
             loaded into the kind node, where containerd files an import under its tag only, so
             it names a tag, which must be one images.sh pins by digest (NAME_image=TAG beside
             NAME_digest=sha256:...), as e2e-up.sh and stunner/up.sh pull it.

Each option needs at least one file. An unknown option is an error.
"""
import argparse
import re
import sys
from pathlib import Path

import yaml

DIGEST = re.compile(r"@sha256:[0-9a-f]{64}$")
MONOREPO = "git.askedin.com/askedin/askedin-monorepo/"
SANDBOX_BUILD = "ulw/"


def images(node):
    if isinstance(node, dict):
        for key, value in node.items():
            if key == "image" and isinstance(value, str):
                yield value
            else:
                yield from images(value)
    elif isinstance(node, list):
        for item in node:
            yield from images(item)


def file_images(path: Path):
    with path.open(encoding="utf-8") as f:
        for doc in yaml.safe_load_all(f):
            yield from images(doc)


def pinned_tags(images_sh: Path) -> set[str]:
    text = images_sh.read_text(encoding="utf-8")
    tags = dict(re.findall(r"^(\w+)_image=(\S+)$", text, re.M))
    digests = dict(re.findall(r"^(\w+)_digest=(sha256:[0-9a-f]{64})$", text, re.M))
    return {tag for name, tag in tags.items() if name in digests}


def check(askedin, host, sandbox, tags: set[str]) -> list[str]:
    errors = []
    for path in askedin:
        for image in file_images(path):
            if not image.startswith(MONOREPO) and not DIGEST.search(image):
                errors.append(f"{path}: {image} is not pinned by digest (@sha256:...)")
    for path in host:
        for image in file_images(path):
            if not image.startswith(SANDBOX_BUILD) and not DIGEST.search(image):
                errors.append(f"{path}: {image} is not pinned by digest (@sha256:...)")
    for path in sandbox:
        for image in file_images(path):
            if image.startswith(SANDBOX_BUILD) or DIGEST.search(image) or image in tags:
                continue
            errors.append(
                f"{path.name}: {image} is loaded into the sandbox by tag, but images.sh pins "
                "no digest for that tag"
            )
    return errors


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--images-sh", type=Path, required=True)
    for group in ("askedin", "host", "sandbox"):
        parser.add_argument(f"--{group}", type=Path, nargs="+", required=True)
    args = parser.parse_args(argv)  # an unknown option or a missing file list exits 2
    for path in [args.images_sh, *args.askedin, *args.host, *args.sandbox]:
        if not path.is_file():
            parser.error(f"{path}: no such file")
    errors = check(args.askedin, args.host, args.sandbox, pinned_tags(args.images_sh))
    for e in errors:
        print(f"check-image-pins: {e}", file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
