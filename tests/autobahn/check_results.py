#!/usr/bin/env python3
"""Usage: check_results.py <index.json> <min-cases>

Passes only if the Autobahn report holds at least <min-cases> cases and every case's behavior
and behaviorClose are on the allowlist. Anything else Autobahn can report (FAILED, WRONG CODE,
UNCLEAN, FAILED BY CLIENT, or a verdict added in a later release) fails it. UNIMPLEMENTED is
allowed only for the compression cases, 12.x and 13.x: permessage-deflate is never agreed."""
import collections
import json
import sys

ALLOWED = {"OK", "NON-STRICT", "INFORMATIONAL"}
COMPRESSION = {"12", "13"}


def problems(results, min_cases):
    counts = collections.Counter()
    bad = []
    total = 0
    for cases in results.values():
        for case, r in cases.items():
            total += 1
            allowed = ALLOWED | ({"UNIMPLEMENTED"} if case.split(".")[0] in COMPRESSION else set())
            for field in ("behavior", "behaviorClose"):
                verdict = r.get(field)
                counts[f"{field}={verdict}"] += 1
                if verdict not in allowed:
                    bad.append(f"{case}: {field} {verdict}")
    if total < min_cases:
        bad.append(f"{total} cases, expected at least {min_cases}")
    return total, counts, bad


def main(argv):
    if len(argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    try:
        with open(argv[1], encoding="utf-8") as f:
            results = json.load(f)
        total, counts, bad = problems(results, int(argv[2]))
    except (OSError, ValueError, AttributeError) as e:
        print(f"unreadable report: {e}", file=sys.stderr)
        return 1
    print(f"total={total}", " ".join(f"{k}:{v}" for k, v in sorted(counts.items())))
    for line in bad:
        print(line, file=sys.stderr)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
