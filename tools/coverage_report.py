#!/usr/bin/env python3
"""Sums `llvm-cov export -summary-only` by directory and holds each top-level directory to its
floor (ADR-0074).

Usage: coverage_report.py <repo-root> <export.json> <floors.txt> <summary.json> <summary.md>

A floor file line is `<top-level dir> <line %> <branch %>`; `#` starts a comment. A directory
without a line is reported and not held to anything, so a new one is visible from its first run.
"""
import json
import sys
from pathlib import PurePosixPath


def component(rel: PurePosixPath) -> str:
    """The directory a file belongs to: everything before its first src/ or include/, so
    apps/gateway/src/x.cpp is apps/gateway and infra/storage/s3/include/... infra/storage/s3."""
    parts = rel.parts[:-1]
    for i, part in enumerate(parts):
        if part in ("src", "include"):
            return "/".join(parts[:i]) or parts[0]
    return "/".join(parts) or "."


def pct(covered: int, count: int) -> float:
    return 100.0 * covered / count if count else 100.0


def read_floors(path: str) -> dict[str, tuple[int, int]]:
    floors = {}
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, start=1):
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            fields = line.split()
            if len(fields) != 3:
                raise SystemExit(f"{path}:{n}: expected '<dir> <line %> <branch %>'")
            floors[fields[0]] = (int(fields[1]), int(fields[2]))
    return floors


def main() -> int:
    root, export, floors_path, summary_json, summary_md = sys.argv[1:6]
    root_path = PurePosixPath(root)
    with open(export, encoding="utf-8") as f:
        data = json.load(f)

    tops: dict[str, list[int]] = {}
    parts: dict[str, list[int]] = {}
    for entry in data["data"][0]["files"]:
        path = PurePosixPath(entry["filename"])
        try:
            rel = path.relative_to(root_path)
        except ValueError:
            continue  # system headers
        if rel.parts[0] in ("tests", "third_party", "build"):
            continue
        s = entry["summary"]
        row = [s["lines"]["covered"], s["lines"]["count"],
               s["branches"]["covered"], s["branches"]["count"]]
        for key, table in ((rel.parts[0], tops), (component(rel), parts)):
            acc = table.setdefault(key, [0, 0, 0, 0])
            for i, v in enumerate(row):
                acc[i] += v

    floors = read_floors(floors_path)
    failures = []
    summary = {}
    lines = ["## Coverage", "",
             "Unit and integration labels; tests, third-party and generated code excluded. "
             "Floors: ADR-0074, tools/coverage-floors.txt.", "",
             "| Directory | Lines | Line % | Branches | Branch % | Floor (line / branch) |",
             "|---|---:|---:|---:|---:|---|"]
    total = [0, 0, 0, 0]
    for top in sorted(tops):
        lc, lt, bc, bt = tops[top]
        for i, v in enumerate(tops[top]):
            total[i] += v
        line_pct, branch_pct = pct(lc, lt), pct(bc, bt)
        floor = floors.get(top)
        mark = ""
        if floor:
            if line_pct < floor[0]:
                failures.append(f"{top}: line coverage {line_pct:.2f}% is below its floor {floor[0]}%")
                mark = " **below**"
            if branch_pct < floor[1]:
                failures.append(
                    f"{top}: branch coverage {branch_pct:.2f}% is below its floor {floor[1]}%")
                mark = " **below**"
        floor_text = f"{floor[0]} / {floor[1]}{mark}" if floor else "none"
        lines.append(f"| {top} | {lc}/{lt} | {line_pct:.1f} | {bc}/{bt} | {branch_pct:.1f} "
                     f"| {floor_text} |")
        summary[top] = {"lines": [lc, lt], "branches": [bc, bt]}
    lines.append(f"| **total** | {total[0]}/{total[1]} | {pct(total[0], total[1]):.1f} "
                 f"| {total[2]}/{total[3]} | {pct(total[2], total[3]):.1f} | |")
    for top in sorted(set(floors) - set(tops)):
        failures.append(f"{top}: has a floor but no measured sources")

    lines += ["", "<details><summary>By component</summary>", "",
              "| Component | Line % | Branch % |", "|---|---:|---:|"]
    for part in sorted(parts):
        lc, lt, bc, bt = parts[part]
        lines.append(f"| {part} | {pct(lc, lt):.1f} | {pct(bc, bt):.1f} |")
    lines += ["", "</details>", ""]
    if failures:
        lines += ["**Below the floor:**", ""] + [f"- {f}" for f in failures] + [""]

    with open(summary_md, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    with open(summary_json, "w", encoding="utf-8") as f:
        json.dump({"directories": summary,
                   "components": {k: {"lines": v[:2], "branches": v[2:]}
                                  for k, v in parts.items()}}, f, indent=1, sort_keys=True)
    for failure in failures:
        print(f"coverage: {failure}", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
