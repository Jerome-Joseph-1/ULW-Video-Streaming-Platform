#!/usr/bin/env python3
"""Runs check_results.py on synthetic reports: it must pass a clean one and fail the rest."""
import json
import subprocess
import sys
import tempfile
from pathlib import Path

CHECKER = Path(__file__).resolve().parent / "check_results.py"


def case(behavior, close="OK"):
    return {"behavior": behavior, "behaviorClose": close}


def good():
    return {"1.1.1": case("OK"), "6.4.1": case("NON-STRICT"), "7.1.6": case("INFORMATIONAL"),
            "12.1.1": case("UNIMPLEMENTED", "UNIMPLEMENTED"),
            "13.1.1": case("UNIMPLEMENTED", "UNIMPLEMENTED")}


def with_case(name, r):
    cases = good()
    cases[name] = r
    return {"ulw": cases}


# (name, report, minimum cases, expected exit status)
CASES = [
    ("clean", {"ulw": good()}, 5, 0),
    ("wrong code on close", with_case("7.9.1", case("OK", "WRONG CODE")), 5, 1),
    ("unclean close", with_case("5.19", case("OK", "UNCLEAN")), 5, 1),
    ("failed by client", with_case("2.10", case("FAILED BY CLIENT")), 5, 1),
    ("failed", with_case("9.1.1", case("FAILED", "FAILED")), 5, 1),
    ("unimplemented outside compression", with_case("1.2.1", case("UNIMPLEMENTED")), 5, 1),
    ("missing verdict", with_case("1.2.2", {"behavior": "OK"}), 5, 1),
    ("too few cases", {"ulw": good()}, 517, 1),
    ("empty", {}, 517, 1),
    ("agent without cases", {"ulw": {}}, 517, 1),
]


def main():
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        for name, report, minimum, expected in CASES:
            path = Path(tmp) / "index.json"
            path.write_text(json.dumps(report), encoding="utf-8")
            status = subprocess.run([sys.executable, str(CHECKER), str(path), str(minimum)],
                                    capture_output=True, check=False).returncode
            if status != expected:
                print(f"{name}: exit {status}, expected {expected}", file=sys.stderr)
                failures += 1
        path = Path(tmp) / "absent.json"
        if subprocess.run([sys.executable, str(CHECKER), str(path), "1"],
                          capture_output=True, check=False).returncode != 1:
            print("absent report: expected exit 1", file=sys.stderr)
            failures += 1
    print(f"{len(CASES) + 1 - failures}/{len(CASES) + 1} checks as expected")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
