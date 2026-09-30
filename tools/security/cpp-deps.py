#!/usr/bin/env python3
"""Writes the vendored C and C++ dependencies as osv-scanner's own lockfile format, so they are
checked against OSV like the Cargo, npm and pip lock files (docs/adr/0072).

Each tarball cmake/Dependencies.cmake pins is looked up below by its SHA-256 and written as its
upstream repository and the commit its tag names. OSV records C and C++ vulnerabilities by
commit ranges of the upstream repository, so a commit is what matches. A tarball in
Dependencies.cmake that this table lacks, or whose hash differs from the table's, fails the
script: updating a dependency means updating its row here as well.

Usage: tools/security/cpp-deps.py OUT.json
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
DEPENDENCIES = ROOT / "cmake" / "Dependencies.cmake"

# tarball: (sha256, repository, tag, commit). llhttp's tarball is its release/v9.2.1 branch,
# the generated C of the v9.2.1 source tag; the source tag's commit is the one on the history
# OSV's ranges are drawn over.
PINNED = {
    "llhttp-9.2.1.tar.gz": (
        "3c163891446e529604b590f9ad097b2e98b5ef7e4d3ddcf1cf98b62ca668f23e",
        "https://github.com/nodejs/llhttp", "v9.2.1",
        "b0b279fb5a617ab3bc2fc11c5f8bd937aac687c1"),
    "googletest-1.15.2.tar.gz": (
        "7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926",
        "https://github.com/google/googletest", "v1.15.2",
        "b514bdc898e2951020cbdca1304b75f5950d1f59"),
    "srt-1.5.4.tar.gz": (
        "d0a8b600fe1b4eaaf6277530e3cfc8f15b8ce4035f16af4a5eb5d4b123640cdd",
        "https://github.com/Haivision/srt", "v1.5.4",
        "a8c6b65520f814c5bd8f801be48c33ceece7c4a6"),
}

DECLARED = re.compile(
    r"URL\s+\$\{ULW_THIRD_PARTY_DIR\}/(\S+)\s+URL_HASH\s+SHA256=([0-9a-f]{64})")


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    declared = DECLARED.findall(DEPENDENCIES.read_text(encoding="utf-8"))
    if not declared:
        print(f"{DEPENDENCIES}: no URL/URL_HASH pairs found; update DECLARED", file=sys.stderr)
        return 1
    errors = []
    packages = []
    for tarball, sha256 in declared:
        if tarball not in PINNED:
            errors.append(f"{tarball}: not in tools/security/cpp-deps.py")
            continue
        pinned_sha, repo, tag, commit = PINNED[tarball]
        if pinned_sha != sha256:
            errors.append(f"{tarball}: Dependencies.cmake has {sha256}, cpp-deps.py {pinned_sha}")
            continue
        packages.append({"package": {"name": repo, "commit": commit}})
        print(f"{tarball}: {repo} {tag} {commit}")
    for e in errors:
        print(f"cpp-deps: {e}", file=sys.stderr)
    if errors:
        return 1
    lockfile = {"results": [{"source": {"path": "cmake/Dependencies.cmake", "type": "lockfile"},
                             "packages": packages}]}
    Path(sys.argv[1]).write_text(json.dumps(lockfile, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
