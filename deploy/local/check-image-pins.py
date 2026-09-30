#!/usr/bin/env python3
"""Usage: check-image-pins.py IMAGES_SH [--loaded RENDERED.yaml...] -- FILE...

Every image a manifest names is pinned by digest (docs/adr/0072), which is what lets Trivy's
KSV-0125 trust a whole registry such as docker.io (tools/security/trivy-data/registries.yaml).
Every `image:` value in each FILE (Kubernetes manifests, compose, kind and Woodpecker files) must
carry @sha256:<64 hex>, except:

  ulw/...                                   the sandbox's own builds, never pulled;
  git.askedin.com/askedin/askedin-monorepo  this repository's own builds, which Woodpecker pushes
                                            under the branch's name (deploy/askedin/woodpecker.yml)
                                            and the overlays follow by that name.

The --loaded files are the sandbox's rendered kustomizations. Their upstream images are loaded
into the kind node, where containerd files an import under its tag only, so they name a tag; each
such tag must be one IMAGES_SH pins by digest (NAME_image=TAG with NAME_digest=sha256:...), which
e2e-up.sh and stunner/up.sh pull by that digest before loading.
"""
import re
import sys
from pathlib import Path

import yaml

DIGEST = re.compile(r"@sha256:[0-9a-f]{64}$")
OWN = ("ulw/", "git.askedin.com/askedin/askedin-monorepo/")


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


def main() -> int:
    args = sys.argv[1:]
    if "--" not in args or not args:
        print(__doc__, file=sys.stderr)
        return 2
    split = args.index("--")
    head, files = args[:split], [Path(p) for p in args[split + 1 :]]
    images_sh, loaded = Path(head[0]), [Path(p) for p in head[2:]] if head[1:2] == ["--loaded"] else []
    tags = pinned_tags(images_sh)
    errors = []
    for path in files:
        for image in file_images(path):
            if not image.startswith(OWN) and not DIGEST.search(image):
                errors.append(f"{path}: {image} is not pinned by digest (@sha256:...)")
    for path in loaded:
        for image in file_images(path):
            if image.startswith(OWN) or DIGEST.search(image):
                continue
            if image not in tags:
                errors.append(
                    f"{path.name}: {image} is loaded into the sandbox by tag, but "
                    f"{images_sh.name} pins no digest for that tag"
                )
    for e in errors:
        print(f"check-image-pins: {e}", file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
