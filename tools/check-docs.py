#!/usr/bin/env python3
"""Checks docs/adr (numbering, required sections, supersession links) and that every gateway
route has a row in the integration guide."""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ADR_DIR = ROOT / "docs" / "adr"
NAME = re.compile(r"^(\d{4})-[a-z0-9]+(?:-[a-z0-9]+)*\.md$")
REQUIRED = ("## Context", "## Options", "## Decision", "## Consequences")
ROUTES = ROOT / "apps" / "gateway" / "src" / "routes.hpp"
GUIDE = ROOT / "docs" / "integration"
# Where Askedin's teams look up an endpoint; each route needs a table row, `METHOD /path`, in one.
ROUTE_PAGES = ("uploads.md", "videos-and-playback.md", "live.md", "operations-contract.md")
ROUTE_ENTRY = re.compile(r'\.method\s*=\s*http::Method::(\w+)\s*,\s*\.pattern\s*=\s*"([^"]+)"')


def check_routes() -> list[str]:
    if not ROUTES.is_file():
        return []
    routes = [(m.upper(), p) for m, p in ROUTE_ENTRY.findall(ROUTES.read_text(encoding="utf-8"))]
    if not routes:
        # A reformatted table must not turn this check into one that always passes.
        return [f"{ROUTES.relative_to(ROOT)}: no routes found; update ROUTE_ENTRY"]
    rows = []
    for name in ROUTE_PAGES:
        page = GUIDE / name
        if page.is_file():
            rows += [line for line in page.read_text(encoding="utf-8").splitlines()
                     if line.startswith("|")]
    pages = ", ".join(ROUTE_PAGES)
    return [f"{method} {pattern}: no table row `{method} {pattern}` in docs/integration ({pages})"
            for method, pattern in routes
            if not any(f"`{method} {pattern}`" in row for row in rows)]


def main() -> int:
    errors = check_routes()
    if not ADR_DIR.is_dir():
        for e in errors:
            print(f"check-docs: {e}", file=sys.stderr)
        return 1 if errors else 0
    numbers = set()
    supersessions = []
    for path in sorted(ADR_DIR.glob("*.md")):
        if path.name == "README.md":
            continue
        m = NAME.match(path.name)
        if not m:
            errors.append(f"{path.name}: expected NNNN-lowercase-slug.md")
            continue
        num = int(m.group(1))
        numbers.add(num)
        text = path.read_text(encoding="utf-8")
        first = text.splitlines()[0] if text else ""
        if not first.startswith(f"# {num:04d}. "):
            errors.append(f"{path.name}: first line must be '# {num:04d}. <title>'")
        status = re.search(r"^Status: (.+)$", text, re.MULTILINE)
        if not status:
            errors.append(f"{path.name}: missing 'Status:' line")
            continue
        superseded = re.match(r"Superseded by (\d{4})", status.group(1))
        if superseded:
            supersessions.append((num, int(superseded.group(1))))
        for heading in REQUIRED:
            if heading not in text:
                errors.append(f"{path.name}: missing section '{heading}'")
        if "## Options" in text and "|" not in text.split("## Options", 1)[1].split("## ", 1)[0]:
            errors.append(f"{path.name}: Options must be a table")

    for expected, actual in enumerate(sorted(numbers), start=1):
        if expected != actual:
            errors.append(f"ADR numbering has a gap: expected {expected:04d}, found {actual:04d}")
            break
    for old, new in supersessions:
        if new not in numbers:
            errors.append(f"{old:04d} is superseded by missing ADR {new:04d}")

    for e in errors:
        print(f"check-docs: {e}", file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
