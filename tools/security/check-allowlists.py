#!/usr/bin/env python3
"""Checks the scanners' allowlists (docs/adr/0072): every entry says why and when it expires,
and no expiry is more than a year out, so nothing is waived for good.

  tools/security/osv-scanner.toml   [[IgnoredVulns]]: id, reason, ignoreUntil
  tools/security/trivyignore.yaml   every entry: id, statement, expired_at, and paths or purls;
                                    a vulnerability (an image's package) by purls only, one per
                                    binary package and each with its version, and for at most
                                    92 days

An entry past its date is left alone here: the scanner stops honouring it and the finding fails
its job, which is the point.
"""
import datetime
import re
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
OSV = ROOT / "tools" / "security" / "osv-scanner.toml"
TRIVY = ROOT / "tools" / "security" / "trivyignore.yaml"
MAX_DAYS = 366
# An image's vulnerability waits on a package upgrade, not on a design decision, so it is looked
# at again within a quarter.
MAX_VULN_DAYS = 92


def as_date(value) -> datetime.date | None:
    if isinstance(value, datetime.datetime):
        return value.date()
    if isinstance(value, datetime.date):
        return value
    if isinstance(value, str):
        try:
            return datetime.date.fromisoformat(value[:10])
        except ValueError:
            return None
    return None


def check_expiry(where: str, value, errors: list[str], days: int = MAX_DAYS) -> None:
    date = as_date(value)
    if date is None:
        errors.append(f"{where}: no expiry date")
    elif date > datetime.date.today() + datetime.timedelta(days=days):
        errors.append(f"{where}: expires {date}, more than {days} days out")


# A purl naming one version of one package: pkg:type/[namespace/]name@version[?qualifiers].
# Trivy reads a purl without a version as every version, so one is required, and a wildcard
# anywhere is refused.
PURL = re.compile(
    r"^pkg:[a-z][a-z0-9.+-]*/"  # type
    r"(?:[^/@?#*\s]+/)*[^/@?#*\s]+"  # namespace and name
    r"@[^/@?#*\s]+"  # version
    r"(?:\?[^#*\s]*)?$"  # qualifiers
)


def check_purl(where: str, purl, errors: list[str]) -> None:
    if not isinstance(purl, str) or not PURL.match(purl):
        errors.append(
            f"{where}: {purl!r} is not one package at one version "
            "(pkg:type/name@version, no wildcard)"
        )


def check_trivy(config: dict, name: str = "trivyignore.yaml") -> list[str]:
    errors: list[str] = []
    for kind, entries in config.items():
        for i, entry in enumerate(entries or []):
            where = f"{name} {kind} entry {i + 1} ({entry.get('id', 'no id')})"
            if not entry.get("id"):
                errors.append(f"{where}: no id")
            if len(str(entry.get("statement", "")).strip()) < 40:
                errors.append(f"{where}: no statement, or one too short to say why")
            if kind == "vulnerabilities":
                purls = entry.get("purls") or []
                if entry.get("paths") or not purls:
                    errors.append(f"{where}: scope it by the binary packages' purls only")
                for purl in purls:
                    check_purl(where, purl, errors)
                check_expiry(where, entry.get("expired_at"), errors, MAX_VULN_DAYS)
                continue
            if not entry.get("paths") and not entry.get("purls"):
                errors.append(f"{where}: applies everywhere; scope it with paths or purls")
            for purl in entry.get("purls") or []:
                check_purl(where, purl, errors)
            check_expiry(where, entry.get("expired_at"), errors)
    return errors


def main() -> int:
    errors: list[str] = []
    if OSV.is_file():
        config = tomllib.loads(OSV.read_text(encoding="utf-8"))
        for i, entry in enumerate(config.get("IgnoredVulns", [])):
            where = f"{OSV.name} entry {i + 1} ({entry.get('id', 'no id')})"
            if not entry.get("id"):
                errors.append(f"{where}: no id")
            if len(str(entry.get("reason", "")).strip()) < 40:
                errors.append(f"{where}: no reason, or one too short to say why")
            check_expiry(where, entry.get("ignoreUntil"), errors)
        if config.get("PackageOverrides"):
            errors.append(f"{OSV.name}: PackageOverrides waive whole packages; use IgnoredVulns")
    if TRIVY.is_file():
        import yaml  # python3-yaml; only needed when the Trivy allowlist exists.

        config = yaml.safe_load(TRIVY.read_text(encoding="utf-8")) or {}
        errors += check_trivy(config, TRIVY.name)
    for e in errors:
        print(f"check-allowlists: {e}", file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
