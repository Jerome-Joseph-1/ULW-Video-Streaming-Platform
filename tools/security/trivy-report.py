#!/usr/bin/env python3
"""Usage: trivy-report.py IGNOREFILE REPORT.json...

Writes a Markdown summary of Trivy image reports (docs/adr/0072), one table per image, to
$GITHUB_STEP_SUMMARY when set and to stdout otherwise. Each REPORT.json is `trivy image
--format json` with no severity filter, no --ignore-unfixed and no ignore file, so nothing is
hidden from the summary.

A vulnerability is listed when either Trivy's severity or NVD's is HIGH or CRITICAL. The rating
matters because for Ubuntu packages Trivy uses Ubuntu's priority, and Ubuntu rates most ffmpeg
CVEs medium or low where NVD says high or critical. Unfixed ones are listed too: for ffmpeg in
universe the only fixes are in Ubuntu Pro's ESM archive, which Trivy's data records as no fix
("affected"), so --ignore-unfixed alone would drop them silently. An entry in the ignore file
is marked, with its expiry, not left out.

Never fails on findings; trivy-image.sh's own Trivy run is the gate.
"""
import collections
import json
import os
import sys
from pathlib import Path

RANK = {"UNKNOWN": 0, "LOW": 1, "MEDIUM": 2, "HIGH": 3, "CRITICAL": 4}
NAME = {v: k for k, v in RANK.items()}
MAX_ROWS = 150


def allowlisted(ignorefile: Path) -> dict[str, str]:
    """Vulnerability id -> expiry, from the ignore file's `vulnerabilities` entries."""
    if not ignorefile.is_file():
        return {}
    import yaml  # python3-yaml, which the jobs that run this already install.

    config = yaml.safe_load(ignorefile.read_text(encoding="utf-8")) or {}
    return {
        str(e.get("id")): str(e.get("expired_at", "?"))
        for e in config.get("vulnerabilities") or []
    }


def summarise(report: dict, allow: dict[str, str]) -> str:
    image = report.get("ArtifactName", "?")
    os_info = report.get("Metadata", {}).get("OS") or {}
    rows: dict[str, dict] = {}
    for result in report.get("Results") or []:
        for v in result.get("Vulnerabilities") or []:
            trivy = RANK.get(v.get("Severity", "UNKNOWN"), 0)
            nvd = (v.get("VendorSeverity") or {}).get("nvd", 0)
            if max(trivy, nvd) < RANK["HIGH"]:
                continue
            row = rows.setdefault(
                v["VulnerabilityID"],
                {
                    "trivy": trivy,
                    "nvd": nvd,
                    "packages": set(),
                    "installed": set(),
                    "fixed": set(),
                    "status": set(),
                    "title": (v.get("Title") or "").replace("|", "/")[:80],
                },
            )
            row["packages"].add(v.get("PkgName", "?"))
            row["installed"].add(v.get("InstalledVersion", "?"))
            if v.get("FixedVersion"):
                row["fixed"].add(v["FixedVersion"])
            row["status"].add(v.get("Status") or ("fixed" if v.get("FixedVersion") else "affected"))

    def key(item):
        vid, row = item
        return (not row["fixed"], -max(row["trivy"], row["nvd"]), vid)

    fixable = sum(1 for r in rows.values() if r["fixed"])
    out = [f"### Trivy: `{image}`"]
    if os_info:
        out.append(f"\n{os_info.get('Family', '?')} {os_info.get('Name', '?')}.")
    by_rating = collections.Counter(NAME[max(r["trivy"], r["nvd"])] for r in rows.values())
    out.append(
        f"\n{len(rows)} HIGH or CRITICAL by Trivy's or NVD's rating "
        f"({by_rating['CRITICAL']} critical, {by_rating['HIGH']} high): {fixable} with a fixed "
        f"version in the archive (these fail the job when Trivy rates them HIGH or CRITICAL, "
        f"unless allowlisted), {len(rows) - fixable} without one (reported, not failing).\n"
    )
    if not rows:
        return "\n".join(out) + "\n"
    out.append("| Vulnerability | Trivy | NVD | Packages | Installed | Fixed in | Allowlist | Title |")
    out.append("|---|---|---|---|---|---|---|---|")
    for vid, row in sorted(rows.items(), key=key)[:MAX_ROWS]:
        packages = sorted(row["packages"])
        shown = ", ".join(packages[:4]) + (f" (+{len(packages) - 4})" if len(packages) > 4 else "")
        out.append(
            "| {} | {} | {} | {} | {} | {} | {} | {} |".format(
                vid,
                NAME[row["trivy"]],
                NAME.get(row["nvd"], "-") if row["nvd"] else "-",
                shown,
                ", ".join(sorted(row["installed"])),
                ", ".join(sorted(row["fixed"])) or "no fix (" + "/".join(sorted(row["status"])) + ")",
                f"until {allow[vid]}" if vid in allow else "",
                row["title"],
            )
        )
    if len(rows) > MAX_ROWS:
        out.append(f"\n{len(rows) - MAX_ROWS} more in the JSON report.")
    return "\n".join(out) + "\n"


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    allow = allowlisted(Path(sys.argv[1]))
    text = "\n".join(
        summarise(json.loads(Path(p).read_text(encoding="utf-8")), allow) for p in sys.argv[2:]
    )
    target = os.environ.get("GITHUB_STEP_SUMMARY")
    if target:
        with open(target, "a", encoding="utf-8") as f:
            f.write(text + "\n")
    sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
