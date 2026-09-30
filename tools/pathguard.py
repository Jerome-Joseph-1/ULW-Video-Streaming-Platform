"""Keeps a path named on a script's command line inside the places this repository's scripts
read and write: the repository itself, $RUNNER_TEMP when set (GitHub Actions' per-job
directory, e2e.yml's TRIVY_REPORT_DIR), and the system's temporary directory (mktemp -d, as
validate-manifests.sh and osv-scan.sh use).

The path is resolved first, following every symlink and '..', so a link inside the repository
that points elsewhere is judged by where it points. Anything outside is refused with exit 2.

The scripts in deploy/local and tools/security import it by putting this directory on
sys.path, since they run as plain files, not as a package.
"""
import os
import sys
import tempfile
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parent.parent


def allowed_roots() -> list[Path]:
    roots = [REPOSITORY]
    runner_temp = os.environ.get("RUNNER_TEMP")
    if runner_temp:
        roots.append(Path(os.path.realpath(runner_temp)))
    roots.append(Path(os.path.realpath(tempfile.gettempdir())))
    return roots


def inside(path: str | os.PathLike) -> Path:
    """The resolved path, or exit 2 when it lies outside every allowed root."""
    resolved = Path(os.path.realpath(path))
    roots = allowed_roots()
    for root in roots:
        if resolved.is_relative_to(root):
            return resolved
    prog = os.path.basename(sys.argv[0]) or "pathguard"
    print(f"{prog}: {os.fspath(path)!r} resolves outside the repository and the temporary "
          f"directories ({', '.join(map(str, roots))}); refused", file=sys.stderr)
    sys.exit(2)
