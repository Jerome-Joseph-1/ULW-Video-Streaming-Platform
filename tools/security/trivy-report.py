#!/usr/bin/env python3
"""Usage: trivy-report.py IGNOREFILE REPORT.json...

Writes a Markdown summary of Trivy image reports (docs/adr/0072), one table per image, to
stdout, and appends the same to $GITHUB_STEP_SUMMARY when that is set. Each REPORT.json is
`trivy image --format json` with no severity filter, no --ignore-unfixed and no ignore file,
so nothing is hidden from the summary.

A vulnerability is listed when either Trivy's severity or NVD's is HIGH or CRITICAL. The rating
matters because for Ubuntu packages Trivy uses Ubuntu's priority, and Ubuntu rates most ffmpeg
CVEs medium or low where NVD says high or critical. Unfixed ones are listed too: for ffmpeg in
universe the only fixes are in Ubuntu Pro's ESM archive, which Trivy's data records as no fix
("affected"), so --ignore-unfixed alone would drop them silently. A finding an ignore-file
entry covers is marked, with the entry's expiry, not left out: an entry covers it when the ids
match, the entry has not expired, and one of its purls names the finding's package (the same
type, namespace and name, the same version if the purl gives one, and the purl's qualifiers
among the package's), as Trivy itself matches.

Never fails on findings; trivy-image.sh's own Trivy run is the gate.
"""
import collections
import datetime
import json
import os
import sys
from pathlib import Path

# tools/pathguard.py, which keeps each path given on the command line inside the repository
# and the temporary directories.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from pathguard import inside  # noqa: E402

RANK = {"UNKNOWN": 0, "LOW": 1, "MEDIUM": 2, "HIGH": 3, "CRITICAL": 4}
NAME = {v: k for k, v in RANK.items()}
MAX_ROWS = 150


def parse_purl(purl: str) -> tuple[str, str | None, dict[str, str]]:
    """(type/namespace/name, version or None, qualifiers)."""
    purl = purl.split("#", 1)[0]
    purl, _, query = purl.partition("?")
    base, _, version = purl.partition("@")
    qualifiers = dict(q.split("=", 1) for q in query.split("&") if "=" in q)
    return base.lower(), version or None, qualifiers


def purl_covers(entry: str, package: str) -> bool:
    e_base, e_version, e_quals = parse_purl(entry)
    p_base, p_version, p_quals = parse_purl(package)
    if e_base != p_base or (e_version is not None and e_version != p_version):
        return False
    return all(p_quals.get(k) == v for k, v in e_quals.items())


def allowlist(ignorefile: Path) -> list[dict]:
    """The ignore file's unexpired `vulnerabilities` entries."""
    if not ignorefile.is_file():
        return []
    import yaml  # python3-yaml, which the jobs that run this already install.

    config = yaml.safe_load(ignorefile.read_text(encoding="utf-8")) or {}
    today = datetime.date.today()
    entries = []
    for e in config.get("vulnerabilities") or []:
        expiry = e.get("expired_at")
        if isinstance(expiry, str):
            expiry = datetime.date.fromisoformat(expiry[:10])
        if isinstance(expiry, datetime.datetime):
            expiry = expiry.date()
        if isinstance(expiry, datetime.date) and expiry >= today:
            entries.append({"id": str(e.get("id")), "purls": e.get("purls") or [], "until": expiry})
    return entries


def covered_until(allow: list[dict], vid: str, purl: str) -> datetime.date | None:
    dates = [
        e["until"]
        for e in allow
        if e["id"] == vid and purl and any(purl_covers(p, purl) for p in e["purls"])
    ]
    return max(dates) if dates else None


def summarise(report: dict, allow: list[dict]) -> str:
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
                    "covered": {},
                    "title": (v.get("Title") or "").replace("|", "/")[:80],
                },
            )
            row["packages"].add(v.get("PkgName", "?"))
            purl = (v.get("PkgIdentifier") or {}).get("PURL", "")
            until = covered_until(allow, v["VulnerabilityID"], purl)
            if until:
                row["covered"][v.get("PkgName", "?")] = until
            row["installed"].add(v.get("InstalledVersion", "?"))
            if v.get("FixedVersion"):
                row["fixed"].add(v["FixedVersion"])
            row["status"].add(v.get("Status") or ("fixed" if v.get("FixedVersion") else "affected"))

    def allowed(row) -> str:
        if not row["covered"]:
            return ""
        until = min(row["covered"].values())
        n, total = len(row["covered"]), len(row["packages"])
        return f"until {until}" + ("" if n == total else f" ({n} of {total} packages)")

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
                allowed(row),
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
    allow = allowlist(inside(sys.argv[1]))
    reports = [inside(p) for p in sys.argv[2:]]
    text = "\n".join(
        summarise(json.loads(p.read_text(encoding="utf-8")), allow) for p in reports
    )
    target = os.environ.get("GITHUB_STEP_SUMMARY")
    if target:
        with open(target, "a", encoding="utf-8") as f:
            f.write(text + "\n")
    sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
