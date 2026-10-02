#!/usr/bin/env python3
"""tools/coverage_report.py, on small llvm-cov exports written here: files are summed per
top-level directory and per component, tests, third-party, build and system files are left out,
a directory below its floor exits 3 and is named, a floor without measured sources fails, a
malformed floors file and a path outside the repository are refused, and the markdown and JSON
summaries carry the numbers (ADR-0080)."""
import importlib.util
import io
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr
from pathlib import PurePosixPath
from unittest import mock

HERE = pathlib.Path(__file__).resolve().parent
SCRIPT = HERE / "coverage_report.py"


def load(name: str, path: pathlib.Path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


report = load("coverage_report", SCRIPT)

ROOT = "/src/ulw"


def entry(rel: str, lines: tuple[int, int], branches: tuple[int, int], root: str = ROOT):
    filename = rel if rel.startswith("/") else f"{root}/{rel}"
    return {"filename": filename,
            "summary": {"lines": {"covered": lines[0], "count": lines[1]},
                        "branches": {"covered": branches[0], "count": branches[1]}}}


# apps: 90/100 lines, 40/50 branches over two components; core: 30/40, 0/0 (no branches).
# Every excluded file carries numbers that would show if it were counted.
FILES = [
    entry("apps/gateway/src/server.cpp", (60, 60), (30, 30)),
    entry("apps/gateway/include/ulw/gateway/server.hpp", (10, 20), (0, 10)),
    entry("apps/chat/src/main.cpp", (20, 20), (10, 10)),
    entry("core/log.cpp", (30, 40), (0, 0)),
    entry("tests/unit/x_test.cpp", (1, 1000), (1, 1000)),
    entry("third_party/doctest.h", (1, 1000), (1, 1000)),
    entry("build/ci/build_info.cpp", (1, 1000), (1, 1000)),
    entry("/usr/include/c++/14/vector", (1, 1000), (1, 1000)),
]


class ComponentTest(unittest.TestCase):
    def test_everything_before_the_first_src_or_include(self):
        cases = {
            "apps/gateway/src/server.cpp": "apps/gateway",
            "infra/storage/s3/include/ulw/s3/client.hpp": "infra/storage/s3",
            "infra/storage/s3/src/a/src/b.cpp": "infra/storage/s3",
        }
        for path, expected in cases.items():
            with self.subTest(path=path):
                self.assertEqual(report.component(PurePosixPath(path)), expected)

    def test_a_top_level_src_is_its_own_component(self):
        self.assertEqual(report.component(PurePosixPath("src/x.cpp")), "src")

    def test_without_src_or_include_the_directory_is_the_component(self):
        self.assertEqual(report.component(PurePosixPath("core/sub/log.cpp")), "core/sub")
        self.assertEqual(report.component(PurePosixPath("top.cpp")), ".")


class PctTest(unittest.TestCase):
    def test_a_fraction(self):
        self.assertAlmostEqual(report.pct(1, 3), 100.0 / 3)
        self.assertEqual(report.pct(0, 4), 0.0)

    def test_nothing_to_cover_is_fully_covered(self):
        self.assertEqual(report.pct(0, 0), 100.0)


class Workspace(unittest.TestCase):
    """A temporary directory, which tools/pathguard.py lets the script read and write."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = pathlib.Path(self.tmp.name)

    def write(self, name: str, text: str) -> str:
        path = self.dir / name
        path.write_text(text, encoding="utf-8")
        return str(path)


class ReadFloorsTest(Workspace):
    def test_lines_comments_and_blanks(self):
        path = self.write("floors.txt", "# header\n\napps 86 79\ncore 91 83  # trailing\n   \n")
        self.assertEqual(report.read_floors(path), {"apps": (86, 79), "core": (91, 83)})

    def test_a_line_with_the_wrong_field_count_is_refused_with_its_number(self):
        path = self.write("floors.txt", "apps 86 79\ncore 91\n")
        with self.assertRaises(SystemExit) as raised:
            report.read_floors(path)
        self.assertEqual(str(raised.exception),
                         f"{path}:2: expected '<dir> <line %> <branch %>'")

    def test_a_floor_that_is_not_a_number_is_refused(self):
        path = self.write("floors.txt", "apps high 79\n")
        with self.assertRaises(ValueError):
            report.read_floors(path)

    def test_a_floors_file_outside_the_repository_is_refused(self):
        stderr = io.StringIO()
        with redirect_stderr(stderr), self.assertRaises(SystemExit) as raised:
            report.read_floors("/etc/hostname")
        self.assertEqual(raised.exception.code, 2)
        self.assertIn("refused", stderr.getvalue())


class MainTest(Workspace):
    def run_report(self, floors: str, files=None, export_path: str | None = None,
                   root: str = ROOT):
        export = export_path or self.write(
            "export.json", json.dumps({"data": [{"files": FILES if files is None else files}]}))
        self.md = self.dir / "summary.md"
        self.json = self.dir / "summary.json"
        argv = ["coverage_report.py", root, export, self.write("floors.txt", floors),
                str(self.json), str(self.md)]
        stderr = io.StringIO()
        with mock.patch.object(sys, "argv", argv), redirect_stderr(stderr):
            status = report.main()
        self.stderr = stderr.getvalue()
        return status

    def markdown(self) -> str:
        return self.md.read_text(encoding="utf-8")

    def summary(self) -> dict:
        return json.loads(self.json.read_text(encoding="utf-8"))

    def test_sums_per_directory_and_component_and_leaves_out_the_rest(self):
        self.assertEqual(self.run_report("apps 90 80\ncore 75 0\n"), 0)
        self.assertEqual(self.stderr, "")
        self.assertEqual(self.summary(), {
            "directories": {"apps": {"lines": [90, 100], "branches": [40, 50]},
                            "core": {"lines": [30, 40], "branches": [0, 0]}},
            "components": {"apps/chat": {"lines": [20, 20], "branches": [10, 10]},
                           "apps/gateway": {"lines": [70, 80], "branches": [30, 40]},
                           "core": {"lines": [30, 40], "branches": [0, 0]}},
        })
        md = self.markdown()
        self.assertIn("| apps | 90/100 | 90.0 | 40/50 | 80.0 | 90 / 80 |", md)
        self.assertIn("| core | 30/40 | 75.0 | 0/0 | 100.0 | 75 / 0 |", md)
        self.assertIn("| **total** | 120/140 | 85.7 | 40/50 | 80.0 | |", md)
        self.assertIn("| apps/gateway | 87.5 | 75.0 |", md)
        self.assertIn("| apps/chat | 100.0 | 100.0 |", md)
        for excluded in ("tests", "third_party", "build", "usr"):
            self.assertNotIn(f"| {excluded}", md)
        self.assertNotIn("Below the floor", md)

    def test_a_directory_without_a_floor_is_reported_and_not_held(self):
        self.assertEqual(self.run_report("apps 90 80\n"), 0)
        self.assertIn("| core | 30/40 | 75.0 | 0/0 | 100.0 | none |", self.markdown())

    def test_a_line_floor_miss_exits_3_and_names_the_directory(self):
        self.assertEqual(self.run_report("apps 91 80\ncore 75 0\n"), report.BELOW_FLOOR)
        self.assertEqual(report.BELOW_FLOOR, 3)
        failure = "apps: line coverage 90.00% is below its floor 91%"
        self.assertIn(f"coverage: {failure}", self.stderr)
        md = self.markdown()
        self.assertIn("| 91 / 80 **below** |", md)
        self.assertIn(f"**Below the floor:**\n\n- {failure}\n", md)

    def test_a_branch_floor_miss_exits_3(self):
        self.assertEqual(self.run_report("apps 90 81\n"), 3)
        self.assertEqual(self.stderr.strip(),
                         "coverage: apps: branch coverage 80.00% is below its floor 81%")
        self.assertIn("| 90 / 81 **below** |", self.markdown())

    def test_both_misses_are_listed(self):
        self.assertEqual(self.run_report("apps 95 95\n"), 3)
        self.assertEqual(len(self.stderr.strip().splitlines()), 2)

    def test_a_floor_without_measured_sources_fails(self):
        self.assertEqual(self.run_report("apps 0 0\nos 46 24\n"), 3)
        self.assertIn("coverage: os: has a floor but no measured sources", self.stderr)
        self.assertIn("- os: has a floor but no measured sources", self.markdown())

    def test_an_empty_export_writes_an_all_zero_total(self):
        self.assertEqual(self.run_report("", files=[]), 0)
        self.assertIn("| **total** | 0/0 | 100.0 | 0/0 | 100.0 | |", self.markdown())
        self.assertEqual(self.summary(), {"directories": {}, "components": {}})

    def test_files_under_another_root_are_not_counted(self):
        self.assertEqual(self.run_report("", root="/elsewhere"), 0)
        self.assertEqual(self.summary()["directories"], {})

    def test_an_export_outside_the_repository_is_refused_before_anything_is_written(self):
        with self.assertRaises(SystemExit) as raised:
            self.run_report("", export_path="/etc/hostname")
        self.assertEqual(raised.exception.code, 2)
        self.assertFalse(self.md.exists())
        self.assertFalse(self.json.exists())

    def test_a_summary_outside_the_repository_is_refused(self):
        export = self.write("export.json", json.dumps({"data": [{"files": FILES}]}))
        floors = self.write("floors.txt", "")
        argv = ["coverage_report.py", ROOT, export, floors, str(self.dir / "s.json"),
                "/etc/ulw-summary.md"]
        with mock.patch.object(sys, "argv", argv), redirect_stderr(io.StringIO()), \
                self.assertRaises(SystemExit) as raised:
            report.main()
        self.assertEqual(raised.exception.code, 2)
        self.assertFalse((self.dir / "s.json").exists())


class CommandLineTest(Workspace):
    """The exit codes tools/coverage.sh tells apart: 0, 3 for a floor miss, anything else an
    error in the report itself."""

    def run_script(self, floors: str, export: str | None = None):
        export = export or self.write(
            "export.json", json.dumps({"data": [{"files": FILES}]}))
        args = [sys.executable, str(SCRIPT), ROOT, export, self.write("floors.txt", floors),
                str(self.dir / "summary.json"), str(self.dir / "summary.md")]
        env = {k: v for k, v in os.environ.items() if not k.startswith("COVERAGE")}
        return subprocess.run(args, capture_output=True, text=True, env=env, check=False)

    def test_within_the_floors_exits_0(self):
        result = self.run_script("apps 90 80\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.dir / "summary.md").exists())

    def test_below_a_floor_exits_3(self):
        result = self.run_script("apps 99 0\n")
        self.assertEqual(result.returncode, 3, result.stderr)
        self.assertIn("apps: line coverage 90.00% is below its floor 99%", result.stderr)

    def test_a_path_outside_exits_2(self):
        result = self.run_script("", export="/etc/hostname")
        self.assertEqual(result.returncode, 2)
        self.assertIn("refused", result.stderr)

    def test_a_malformed_floors_file_is_neither_0_nor_3(self):
        result = self.run_script("apps 90\n")
        self.assertNotIn(result.returncode, (0, 3))
        self.assertIn("expected '<dir> <line %> <branch %>'", result.stderr)


if __name__ == "__main__":
    unittest.main()
