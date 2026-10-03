#!/usr/bin/env python3
"""tools/mutate.py, on tiny C++ files written to a temporary directory: the masking of literals
and comments, the mutants each operator generates and their stable ids, applying a mutant and
restoring the file byte for byte, the test command's refusals, the result of each mutant
(unbuildable, killed, timeout, survived) with its record and the score, sampling, --only,
--lines, --budget-s, the stop signals, and the process-group handling of a build or test run.
No build runs: ninja and the test binary are replaced by a fake that decides from the mutated
source what a real build and test would have said."""
import contextlib
import importlib.util
import io
import json
import os
import pathlib
import runpy
import signal
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

HERE = pathlib.Path(__file__).resolve().parent
SCRIPT = HERE / "mutate.py"


def load(name: str, path: pathlib.Path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


mutate = load("mutate", SCRIPT)

STOP_SIGNALS = (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)

# Carriage returns kept on purpose: the restore must give back these exact bytes.
SOURCE = ("int f(int a, int b) {\r\n"
          "    if (a < b && b) {\r\n"
          "        return 7;\r\n"
          "    }\r\n"
          "    return 0;\r\n"
          "}\r\n")


class Workspace(unittest.TestCase):
    """A temporary directory as the working directory, which tools/pathguard.py lets the script
    read and write, with the stop signals' handlers and the hold put back afterwards."""

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.dir = pathlib.Path(tmp.name).resolve()
        previous = os.getcwd()
        os.chdir(self.dir)
        self.addCleanup(os.chdir, previous)
        handlers = {s: signal.getsignal(s) for s in STOP_SIGNALS}
        self.addCleanup(lambda: [signal.signal(s, h) for s, h in handlers.items()])
        self.addCleanup(self.release_hold)

    @staticmethod
    def release_hold():
        mutate.Hold.on = False
        mutate.Hold.signals = []

    def write(self, name: str, text: str) -> pathlib.Path:
        path = self.dir / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(text.encode("utf-8"))
        return path

    def executable(self, name: str) -> pathlib.Path:
        path = self.write(name, "#!/bin/sh\nexit 0\n")
        path.chmod(0o755)
        return path


def mutants(source: str, ops=None):
    """(op, old, new) for each mutant of `source`, in the order generated."""
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp).resolve()
        (root / "x.cpp").write_bytes(source.encode("utf-8"))
        found = mutate.mutants_for(str(root / "x.cpp"), root)
    return [(m["op"], m["old"], m["new"]) for m in found if ops is None or m["op"] in ops]


class MaskTest(unittest.TestCase):
    def test_comments_and_literals_are_blanked_in_place(self):
        lines = ['a < b; // c < d',
                 'x = "p < q" + \'<\';',
                 's = "esc \\" < y";',
                 'r = R"(a < b)";']
        masked = mutate.mask(lines)
        self.assertEqual([len(m) for m in masked], [len(line) for line in lines])
        self.assertEqual(masked[0], "a < b;         ")
        self.assertEqual(masked[1], 'x = "_____" + \'_\';')
        self.assertNotIn("<", masked[2])
        self.assertEqual(masked[3], 'r = R"_______";')

    def test_a_block_comment_spans_lines(self):
        masked = mutate.mask(["a /* b < c", "d < e */ f < g", "h /* i */ j"])
        self.assertEqual(masked, ["a         ", "         f < g", "h         j"])

    def test_unterminated_literals_end_at_the_line_end(self):
        self.assertEqual(mutate.mask(['x("abc']), ['x("___'])
        self.assertEqual(mutate.mask(['R"(abc']), ['R"____'])
        # A literal's last character on its line is inside it too: no mutant of the 5.
        self.assertEqual(mutants('s = "5\n'), [])


class DeclarationTest(unittest.TestCase):
    def test_a_type_and_a_name_then_an_initialiser_or_end(self):
        for code in ("int x = 1;", "std::string name{};", "Foo* p(q);", "unsigned  n;"):
            with self.subTest(code=code):
                self.assertTrue(mutate.is_declaration(code))

    def test_statements_are_not_declarations(self):
        for code in ("a = 1;", "foo(bar);", "x;", "int  ;", "a.b = c;", "x <<= 1;", "++i;"):
            with self.subTest(code=code):
                self.assertFalse(mutate.is_declaration(code))


class OperatorTest(unittest.TestCase):
    def test_ror_replaces_a_relational_operator_by_each_other(self):
        self.assertEqual(mutants("bool g = a <= b;", {"ror"}),
                         [("ror", "<=", alt) for alt in ("<", ">", ">=", "==", "!=")])

    def test_unspaced_and_template_angles_are_not_relational(self):
        self.assertEqual(mutants("x = a<b;\nstd::vector<int> v;\n", {"ror"}), [])

    def test_lcr_swaps_and_and_or(self):
        self.assertEqual(mutants("ok = a && b || c;", {"lcr"}),
                         [("lcr", "&&", "||"), ("lcr", "||", "&&")])

    def test_neg_negates_an_if_or_while_condition(self):
        self.assertEqual(mutants("if (a(b)) {}\nwhile (c) {}\n", {"neg"}),
                         [("neg", "(a(b))", "(!(a(b)))"), ("neg", "(c)", "(!(c))")])

    def test_neg_leaves_init_statements_and_unclosed_conditions(self):
        self.assertEqual(mutants("if (auto x = g()) {}\nif (int i = 0; i) {}\nwhile (a &&\n",
                                 {"neg"}), [])

    def test_minmax_swaps_std_and_ranges(self):
        self.assertEqual(mutants("x = std::min(a, std::ranges::max(b, c));", {"minmax"}),
                         [("minmax", "std::min", "std::max"),
                          ("minmax", "std::ranges::max", "std::ranges::min")])

    def test_ret_swaps_booleans_and_deletes_an_early_return(self):
        self.assertEqual(mutants("return true;\nreturn false;\n    return;\n", {"ret"}),
                         [("ret", "return true;", "return false;"),
                          ("ret", "return false;", "return true;"),
                          ("ret", "return;", "")])

    def test_const_steps_integers_keeping_the_suffix(self):
        self.assertEqual(mutants("n = 0;\nm = 10u;\nk = 3z;\n", {"const"}),
                         [("const", "0", "1"),
                          ("const", "10u", "11u"), ("const", "10u", "9u"),
                          ("const", "3z", "4z"), ("const", "3z", "2z")])

    def test_const_leaves_octal_hex_floats_and_identifiers(self):
        self.assertEqual(mutants("a = 010;\nb = 0x1F;\nc = 1.5;\nd = v2;\n", {"const"}), [])

    def test_del_removes_calls_assignments_and_increments(self):
        source = ("    f(x);\n    a = b;\n    i += 2;\n    ++j;\n    k--;\n    p->q(r);\n")
        self.assertEqual([old for _, old, _ in mutants(source, {"del"})],
                         ["f(x);", "a = b;", "i += 2;", "++j;", "k--;", "p->q(r);"])

    def test_del_leaves_everything_else(self):
        source = "\n".join([
            "int x = 1;",                 # a declaration
            "return x;", "break;", "auto y = f();", "const int z = 2;",
            "log_info(\"x\");",            # logging
            "Foo() = default;", "Foo(const Foo&) = delete;",
            "f(a, (b);",                  # unbalanced
            "x == y;",                    # not an assignment
            "g(a,",                       # a statement continued on the next line
            "  b);",
            "f(x)",                       # no semicolon
        ])
        self.assertEqual(mutants(source, {"del"}), [])

    def test_a_statement_after_a_complete_one_or_a_directive_is_not_a_continuation(self):
        source = "#define X 1\nf();\n{\ng();\nlabel:\nh();\n"
        self.assertEqual([old for _, old, _ in mutants(source, {"del"})], ["f();", "g();", "h();"])

    def test_directives_static_asserts_templates_comments_and_strings_are_skipped(self):
        source = ("#if A < B\n"
                  "static_assert(a < b);\n"
                  "template <typename T> bool lt = a < b;\n"
                  "// a < b\n"
                  "s = \"a < b\";\n"
                  "\n")
        self.assertEqual([op for op, _, _ in mutants(source) if op != "del"], [])

    def test_ids_are_file_line_operator_index(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp).resolve()
            (root / "src").mkdir()
            (root / "src" / "x.cpp").write_text("a < b;\nc = 1;\n")
            found = mutate.mutants_for(str(root / "src" / "x.cpp"), root)
        ids = [m["id"] for m in found]
        self.assertEqual(ids[:5], [f"src/x.cpp:1:ror:{i}" for i in range(5)])
        self.assertIn("src/x.cpp:2:const:1", ids)
        self.assertIn("src/x.cpp:2:del:0", ids)
        first = found[0]
        self.assertEqual((first["file"], first["line"], first["col"], first["what"]),
                         ("src/x.cpp", 1, 2, "< -> <="))

    def test_a_file_outside_the_working_directory_is_refused(self):
        with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
            path = pathlib.Path(a) / "x.cpp"
            path.write_text("a < b;\n")
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr), self.assertRaises(SystemExit) as raised:
                mutate.mutants_for(str(path), pathlib.Path(b).resolve())
        self.assertEqual(raised.exception.code, 2)
        self.assertIn("is not under the working directory", stderr.getvalue())
        self.assertIn("refused", stderr.getvalue())


class BalancedEndTest(unittest.TestCase):
    def test_the_matching_parenthesis_or_minus_one(self):
        self.assertEqual(mutate.balanced_end("if (a(b) && c) x", 3), 13)
        self.assertEqual(mutate.balanced_end("if (a(b", 3), -1)


class SampleOrderTest(unittest.TestCase):
    def test_stable_for_a_seed_and_different_across_seeds(self):
        first = mutate.sample_order(1, "k")
        again = mutate.sample_order(1, "k")
        self.assertEqual(first, again)
        self.assertNotEqual(first, mutate.sample_order(2, "k"))
        self.assertEqual(len(first), 64)


class ApplyTest(Workspace):
    def test_apply_returns_the_original_and_the_mutated_text(self):
        path = self.write("x.cpp", SOURCE)
        mutant = next(m for m in mutate.mutants_for(str(path), self.dir)
                      if m["op"] == "ror" and m["new"] == ">=")
        original, mutated = mutate.apply(str(path), mutant)
        self.assertEqual(original, SOURCE)
        self.assertEqual(mutated, SOURCE.replace("a < b", "a >= b"))
        self.assertEqual(path.read_bytes(), SOURCE.encode())  # apply does not write

    def test_a_deleted_statement_keeps_the_line_and_its_carriage_return(self):
        path = self.write("x.cpp", "void g() {\r\n    f(x);\r\n}\r\n")
        mutant = next(m for m in mutate.mutants_for(str(path), self.dir) if m["op"] == "del")
        self.assertEqual(mutate.apply(str(path), mutant)[1], "void g() {\r\n    \r\n}\r\n")

    def test_a_changed_source_is_an_error(self):
        path = self.write("x.cpp", SOURCE)
        mutant = mutate.mutants_for(str(path), self.dir)[0]
        self.write("x.cpp", "\n" + SOURCE)
        with self.assertRaisesRegex(RuntimeError, "source changed"):
            mutate.apply(str(path), mutant)

    def test_write_and_read_round_trip_bytes(self):
        path = self.dir / "y.cpp"
        mutate.write_source(str(path), "a\r\nb é\n")
        self.assertEqual(path.read_bytes(), "a\r\nb é\n".encode())
        self.assertEqual(mutate.read_source(str(path)), "a\r\nb é\n")


class TestCommandTest(Workspace):
    def refused(self, cmd, build_dir=None):
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr), self.assertRaises(SystemExit) as raised:
            mutate.test_command(cmd, build_dir or self.dir / "build")
        self.assertEqual(raised.exception.code, 2)
        return stderr.getvalue()

    def test_ctest_runs_in_the_build_tree_with_known_options(self):
        argv, env = mutate.test_command(["ctest", "-Q", "--output-on-failure"], self.dir / "b")
        self.assertEqual(argv, ["ctest", "--test-dir", str(self.dir / "b"), "-Q",
                                "--output-on-failure"])
        self.assertEqual(env, {})

    def test_other_ctest_options_are_refused(self):
        self.assertIn("ctest option '-R' is not one of", self.refused(["ctest", "-R"]))

    def test_a_bare_program_name_is_refused(self):
        self.assertIn("is neither ctest nor a path", self.refused(["gtest_binary"]))

    def test_a_program_outside_the_repository_and_build_tree_is_refused(self):
        # /bin/sh is outside every root pathguard allows.
        self.assertIn("resolves outside", self.refused(["/bin/sh"]))

    def test_a_program_in_the_temporary_directory_but_not_the_build_tree_is_refused(self):
        other = self.executable("elsewhere/t")
        self.assertIn("is not ctest or inside the repository or the build tree",
                      self.refused([str(other)], build_dir=self.dir / "build"))

    def test_a_program_that_is_not_executable_is_refused(self):
        self.write("build/t", "")
        self.assertIn("is not an executable file", self.refused([str(self.dir / "build/t")]))
        self.assertIn("is not an executable file", self.refused([str(self.dir / "build/none")]))

    def test_googletest_flags_become_environment_variables(self):
        program = self.executable("build/t")
        argv, env = mutate.test_command(
            [str(program), "--gtest_brief=1", "--gtest_filter=A.*:B.c", "--gtest_shuffle"],
            self.dir / "build")
        self.assertEqual(argv, [str(program)])
        self.assertEqual(env, {"GTEST_BRIEF": "1", "GTEST_FILTER": "A.*:B.c",
                               "GTEST_SHUFFLE": "1"})

    def test_a_program_inside_the_repository_is_allowed(self):
        argv, _ = mutate.test_command([str(SCRIPT)], self.dir / "build")
        self.assertEqual(argv, [str(SCRIPT)])

    def test_other_arguments_listing_and_output_files_are_refused(self):
        program = str(self.executable("build/t"))
        self.assertIn("is not a googletest flag", self.refused([program, "--help"]))
        self.assertIn("lists the tests", self.refused([program, "--gtest_list_tests"]))
        self.assertIn("writes a file", self.refused([program, "--gtest_output=xml:/x"]))

    def test_a_flag_file_is_refused(self):
        # A flag file holds flags of its own, --gtest_list_tests and --gtest_output among
        # them, which would pass unchecked.
        program = str(self.executable("build/t"))
        self.write("build/flags", "--gtest_list_tests\n")
        self.assertIn("reads flags from a file",
                      self.refused([program, f"--gtest_flagfile={self.dir / 'build/flags'}"]))
        self.assertIn("reads flags from a file", self.refused([program, "--gtest_flagfile"]))

    def test_refused_googletest_variables_in_the_environment_are_refused(self):
        # The test command inherits the environment, so a GTEST_* variable set there reaches
        # the test binary, or each one ctest runs, as the flag would.
        program = str(self.executable("build/t"))
        for name in ("GTEST_FLAGFILE", "GTEST_LIST_TESTS", "GTEST_OUTPUT"):
            with self.subTest(name=name), mock.patch.dict(os.environ, {name: "x"}):
                self.assertIn(f"the environment sets {name}", self.refused([program]))
                self.assertIn(f"the environment sets {name}", self.refused(["ctest", "-Q"]))
        with mock.patch.dict(os.environ, {"GTEST_COLOR": "no"}):
            argv, _ = mutate.test_command([program], self.dir / "build")
            self.assertEqual(argv, [program])


class FakeBuildAndTest:
    """Stands in for mutate.run. ninja fails when the source holds a negated condition; the test
    times out on `||`, fails with googletest's FAILED lines on `a > b`, fails without them on
    `return 6;`, and passes otherwise. `baseline_build` and `baseline_test` override the first
    build's and the first test's exit code."""

    def __init__(self, path, baseline_build=0, baseline_test=0, during=None):
        self.path, self.calls = path, []
        self.baseline_build, self.baseline_test = baseline_build, baseline_test
        self.during = during  # called on each mutant build, for stops

    def __call__(self, cmd, timeout, env=None, cwd=None):
        src = self.path.read_text()
        self.calls.append((cmd, timeout, env, src))
        if cmd[0] == "ninja":
            builds = sum(1 for c in self.calls if c[0][0] == "ninja")
            if builds == 1:
                return self.baseline_build, "ninja: error: x" * 2000, 1.0
            if self.during and src != SOURCE:
                self.during()
            return (1 if "(!(" in src else 0), "", 2.0
        tests = sum(1 for c in self.calls if c[0][0] != "ninja")
        if tests == 1:
            return self.baseline_test, "baseline", 0.5
        if "||" in src:
            return None, "", 3.0
        if "a > b" in src:
            return 1, "[  FAILED  ] S.b\n[  FAILED  ] S.a\n[  FAILED  ] S.a\n", 0.25
        if "return 6;" in src:
            return 1, "x" * 400 + "segfault", 0.25
        return 0, "", 0.25


class MainTest(Workspace):
    def setUp(self):
        super().setUp()
        self.src = self.write("src/f.cpp", SOURCE)
        self.program = self.executable("build/t")

    def main(self, *args, fake=None, cmd=None):
        files = [] if "--files" in args else ["--files", "src/f.cpp"]
        argv = ["mutate.py", *files, *args]
        if cmd is not False:
            argv += ["--build-dir", "build", "--target", "t", "--", *(cmd or ["build/t"])]
        self.fake = fake or FakeBuildAndTest(self.src)
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.object(sys, "argv", argv), mock.patch.object(mutate, "run", self.fake), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            try:
                status = mutate.main()
            except SystemExit as exited:
                status = exited.code
        return status, out.getvalue(), err.getvalue()

    def records(self):
        with open(self.dir / "r.jsonl", encoding="utf-8") as f:
            return [json.loads(line) for line in f]

    def test_list_prints_the_mutants_and_builds_nothing(self):
        status, out, err = self.main("--list", "--ops", "lcr,neg", cmd=False)
        self.assertEqual(status, 0)
        self.assertEqual(out.splitlines(), ["src/f.cpp:2:lcr:0\t&& -> ||",
                                            "src/f.cpp:2:neg:0\tnegate if condition"])
        self.assertIn("2 mutants of 2", err)
        self.assertEqual(self.fake.calls, [])

    def test_lines_restricts_a_file_to_its_ranges(self):
        status, out, _ = self.main("--list", "--ops", "const", "--lines", "src/f.cpp:4-5,9-9",
                                   cmd=False)
        self.assertEqual(status, 0)
        self.assertEqual(out.splitlines(), ["src/f.cpp:5:const:0\t0 -> 1"])

    def test_each_result_is_classified_recorded_and_scored(self):
        only = ["src/f.cpp:2:neg:0", "src/f.cpp:2:lcr:0", "src/f.cpp:2:ror:1",
                "src/f.cpp:3:const:0", "src/f.cpp:3:const:1"]
        status, out, err = self.main("--only", *only, "--out", "r.jsonl", "--timeout", "9",
                                     "--build-timeout", "99",
                                     cmd=["build/t", "--gtest_brief=1"])
        self.assertEqual(status, 0)
        by_id = {r["id"]: r for r in self.records()}
        self.assertEqual({i: r["status"] for i, r in by_id.items()},
                         {"src/f.cpp:2:neg:0": "unbuildable", "src/f.cpp:2:lcr:0": "timeout",
                          "src/f.cpp:2:ror:1": "killed", "src/f.cpp:3:const:0": "survived",
                          "src/f.cpp:3:const:1": "killed"})
        self.assertEqual(by_id["src/f.cpp:2:ror:1"]["evidence"], "S.a,S.b")
        self.assertEqual(by_id["src/f.cpp:3:const:1"]["evidence"], "x" * 292 + "segfault")
        self.assertEqual(by_id["src/f.cpp:3:const:0"]["evidence"], "")
        unbuildable = by_id["src/f.cpp:2:neg:0"]
        self.assertEqual((unbuildable["build_s"], unbuildable["test_s"]), (2.0, 0.0))
        self.assertEqual(by_id["src/f.cpp:2:ror:1"]["test_s"], 0.2)
        # killed 2 + timeout 1 of 4 that built and ran.
        self.assertIn("summary: {", out)
        self.assertIn("score=75.0%", out)
        self.assertIn("[3/5] unbuildable src/f.cpp:2:neg:0  negate if condition", out)
        self.assertIn("baseline: 0.5s, 5 mutants", err)
        # The file is back, byte for byte, and the target rebuilt from it at the end.
        self.assertEqual(self.src.read_bytes(), SOURCE.encode())
        last = self.fake.calls[-1]
        self.assertEqual(last[0], ["ninja", "-C", str(self.dir / "build"), "-j1", "t"])
        self.assertEqual(last[3], SOURCE.replace("\r\n", "\n"))
        # Mutant builds read the cache only; the test gets its flags as GTEST_* variables.
        builds = [c for c in self.fake.calls if c[0][0] == "ninja"]
        tests = [c for c in self.fake.calls if c[0][0] != "ninja"]
        self.assertTrue(all(c[1] == 99 for c in builds))
        self.assertTrue(all(c[1] == 9 for c in tests))
        self.assertEqual(builds[1][2]["CCACHE_READONLY"], "1")
        self.assertIsNone(builds[0][2])
        self.assertEqual(tests[0][2]["GTEST_BRIEF"], "1")
        self.assertEqual(tests[0][0], [str(self.program)])

    def test_records_are_appended(self):
        self.write("r.jsonl", '{"id": "earlier"}\n')
        self.main("--only", "src/f.cpp:3:const:0", "--out", "r.jsonl")
        self.assertEqual([r["id"] for r in self.records()], ["earlier", "src/f.cpp:3:const:0"])

    def test_without_out_nothing_is_written_and_nothing_scored_is_zero(self):
        status, out, _ = self.main("--only", "src/f.cpp:2:neg:0")
        self.assertEqual(status, 0)
        self.assertIn("summary: {'unbuildable': 1} score=0.0%", out)
        self.assertFalse((self.dir / "r.jsonl").exists())

    def test_unknown_only_ids_exit_naming_them(self):
        status, _, _ = self.main("--only", "src/f.cpp:2:ror:0", "src/f.cpp:99:ror:0")
        self.assertEqual(status, "unknown mutant ids: ['src/f.cpp:99:ror:0']")
        self.assertEqual(self.fake.calls, [])

    def test_sample_takes_the_first_by_seed_and_is_repeatable(self):
        def listed(*args):
            return self.main("--list", "--sample", "3", *args, cmd=False)[1].splitlines()
        first, again, other = listed(), listed(), listed("--seed", "7")
        self.assertEqual(len(first), 3)
        self.assertEqual(first, again)
        self.assertNotEqual(sorted(first), sorted(other))
        everything = {m["id"] for m in mutate.mutants_for("src/f.cpp", self.dir)}
        pool = sorted(everything, key=lambda i: mutate.sample_order(1, i))[:3]
        self.assertEqual(sorted(line.split("\t")[0] for line in first), sorted(pool))

    def test_sample_gives_each_file_its_share_and_a_file_without_mutants_none(self):
        self.write("src/g.cpp", "a < b;\n")   # 5 ror, as f.cpp has; empty.cpp none
        self.write("src/empty.cpp", "\n")
        status, out, _ = self.main("--files", "src/f.cpp", "src/g.cpp", "src/empty.cpp", "--list",
                                   "--ops", "ror", "--sample", "4", cmd=False)
        self.assertEqual(status, 0)
        files = [line.split(":")[0] for line in out.splitlines()]
        self.assertEqual(sorted(files), ["src/f.cpp", "src/f.cpp", "src/g.cpp", "src/g.cpp"])

    def test_a_budget_stops_starting_mutants(self):
        clock = iter([0.0, 0.0, 100.0, 100.0])
        with mock.patch.object(mutate.time, "monotonic", lambda: next(clock)):
            status, out, err = self.main("--budget-s", "10", "--only", "src/f.cpp:3:const:0",
                                         "src/f.cpp:3:const:1")
        self.assertEqual(status, 0)
        self.assertIn("budget spent", err)
        self.assertEqual(out.count("] "), 1)
        self.assertEqual(self.src.read_bytes(), SOURCE.encode())

    def test_build_dir_target_and_test_command_are_required(self):
        status, _, _ = self.main("--build-dir", "build", cmd=False)
        self.assertIn("are required unless --list", status)
        status, _, _ = self.main("--build-dir", "build", "--target", "t", "--", cmd=False)
        self.assertIn("are required unless --list", status)

    def test_a_target_that_is_not_a_ninja_name_is_refused(self):
        status, _, _ = self.main("--build-dir", "build", "--target", "t;x", "--", "build/t",
                                 cmd=False)
        self.assertEqual(status, "--target 't;x' is not a ninja target name")

    def test_the_command_without_a_separator_is_taken_whole(self):
        status, _, _ = self.main("--only", "src/f.cpp:3:const:0", "--build-dir", "build",
                                 "--target", "t", "build/t", cmd=False)
        self.assertEqual(status, 0)

    def test_a_refused_test_command_exits_2_before_any_build(self):
        status, _, err = self.main(cmd=["build/t", "--gtest_output=json"])
        self.assertEqual(status, 2)
        self.assertIn("refused", err)
        self.assertEqual(self.fake.calls, [])

    def test_ctest_is_run_in_the_build_tree(self):
        self.main("--only", "src/f.cpp:3:const:0", cmd=["ctest", "-Q"])
        tests = [c for c in self.fake.calls if c[0][0] != "ninja"]
        self.assertEqual(tests[0][0], ["ctest", "--test-dir", str(self.dir / "build"), "-Q"])

    def test_an_unbuildable_baseline_stops_with_the_tail_of_the_log(self):
        status, _, _ = self.main(fake=FakeBuildAndTest(self.src, baseline_build=1))
        self.assertTrue(status.startswith("the unmutated target does not build:\n"))
        self.assertLess(len(status), 3100)
        self.assertEqual(len(self.fake.calls), 1)

    def test_failing_baseline_tests_stop_the_run(self):
        status, _, _ = self.main(fake=FakeBuildAndTest(self.src, baseline_test=1))
        self.assertEqual(status, "the unmutated tests do not pass:\nbaseline")
        self.assertEqual(len(self.fake.calls), 2)
        self.assertEqual(self.src.read_bytes(), SOURCE.encode())

    def test_a_changed_source_fails_before_writing_and_still_rebuilds(self):
        def change():
            self.write("src/f.cpp", "// edited\n" + SOURCE)

        fake = FakeBuildAndTest(self.src)
        listed = mutate.mutants_for("src/f.cpp", self.dir)
        with mock.patch.object(mutate, "mutants_for", lambda f, root: (change(), listed)[1]):
            with self.assertRaisesRegex(RuntimeError, "source changed"):
                self.main("--only", "src/f.cpp:3:const:0", fake=fake)
        self.assertEqual(self.src.read_text(), "// edited\n" + SOURCE.replace("\r\n", "\n"))
        self.assertEqual(fake.calls[-1][0][0], "ninja")

    def stop_during_the_mutant_build(self, signum):
        """Sends `signum` to this process while a mutant is in the file."""
        return FakeBuildAndTest(self.src, during=lambda: os.kill(os.getpid(), signum))

    def test_sigint_restores_the_file_and_rebuilds(self):
        fake = self.stop_during_the_mutant_build(signal.SIGINT)
        with self.assertRaises(KeyboardInterrupt):
            self.main("--only", "src/f.cpp:3:const:0", "--out", "r.jsonl", fake=fake)
        self.assertEqual(self.src.read_bytes(), SOURCE.encode())
        self.assertEqual(fake.calls[-1][3], SOURCE.replace("\r\n", "\n"))
        self.assertEqual(self.records(), [])
        self.assertFalse(mutate.Hold.on)

    def test_sigterm_and_sighup_raise_stopped(self):
        for signum in (signal.SIGTERM, signal.SIGHUP):
            with self.subTest(signum=signum):
                with self.assertRaisesRegex(mutate.Stopped, signal.Signals(signum).name):
                    self.main("--only", "src/f.cpp:3:const:0",
                              fake=self.stop_during_the_mutant_build(signum))
                self.assertEqual(self.src.read_bytes(), SOURCE.encode())

    def test_a_signal_held_during_the_restore_is_raised_after_it(self):
        writes, held = [], []
        real_write = mutate.write_source

        def write(path, text):
            if text == SOURCE and not held:
                held.append(True)
                # A stop that arrives mid-restore: the handler only records it.
                mutate.on_stop_signal(signal.SIGTERM, None)
                self.assertEqual(mutate.Hold.signals, [signal.SIGTERM])
            writes.append(text)
            real_write(path, text)

        with mock.patch.object(mutate, "write_source", write), \
                self.assertRaisesRegex(mutate.Stopped, "SIGTERM"):
            self.main("--only", "src/f.cpp:3:const:0", "src/f.cpp:3:const:1")
        # Only the first mutant ran; the file was restored once by each finally.
        self.assertEqual(len(writes), 2)
        self.assertEqual(self.src.read_bytes(), SOURCE.encode())
        self.assertEqual(mutate.Hold.signals, [])

    def test_a_signal_held_in_the_final_restore_is_raised_after_the_rebuild(self):
        real_restore = mutate.restore
        seen = []

        def restore(in_progress):
            seen.append(in_progress)
            if len(seen) == 2:
                mutate.Hold.signals.append(signal.SIGINT)
            return real_restore(in_progress)

        with mock.patch.object(mutate, "restore", restore), \
                self.assertRaises(KeyboardInterrupt):
            self.main("--only", "src/f.cpp:3:const:0")
        self.assertIsNone(seen[1])
        self.assertEqual(self.fake.calls[-1][0][0], "ninja")


class HoldTest(Workspace):
    def test_a_signal_outside_the_hold_is_raised(self):
        with self.assertRaises(KeyboardInterrupt):
            mutate.on_stop_signal(signal.SIGINT, None)
        with self.assertRaisesRegex(mutate.Stopped, "SIGHUP"):
            mutate.on_stop_signal(signal.SIGHUP, None)

    def test_restore_writes_releases_and_returns_the_first_held_signal(self):
        path = self.dir / "x.cpp"
        mutate.Hold.on = True
        mutate.on_stop_signal(signal.SIGTERM, None)
        mutate.on_stop_signal(signal.SIGINT, None)
        pending = mutate.restore((str(path), "orig"))
        self.assertEqual(path.read_text(), "orig")
        self.assertIsInstance(pending, mutate.Stopped)
        self.assertFalse(mutate.Hold.on)
        self.assertEqual(mutate.Hold.signals, [])
        mutate.Hold.on = True
        self.assertIsNone(mutate.restore(None))
        self.assertFalse(mutate.Hold.on)

    def test_a_failed_write_still_releases_the_hold(self):
        mutate.Hold.on = True
        with self.assertRaises(OSError):
            mutate.restore((str(self.dir / "missing" / "x.cpp"), "orig"))
        self.assertFalse(mutate.Hold.on)


class FakePopen:
    """A process that never starts: `communicate` replays `outcomes` (bytes, or an exception
    to raise)."""

    def __init__(self, outcomes, returncode=0):
        self.outcomes, self.returncode, self.pid, self.waited = list(outcomes), returncode, 4242, 0

    def communicate(self, timeout=None):
        outcome = self.outcomes.pop(0)
        if isinstance(outcome, BaseException):
            raise outcome
        return outcome, None

    def wait(self):
        self.waited += 1
        return self.returncode


class RunTest(unittest.TestCase):
    def test_a_real_process_in_a_session_of_its_own(self):
        rc, out, elapsed = mutate.run(
            # stdout into a pipe is block-buffered unless PYTHONUNBUFFERED is set (it is in some
            # shells, not on CI), so the child flushes it to pin the order in the merged stream.
            [sys.executable, "-c", "import os, sys; print(os.getsid(0) == os.getpid(), flush=True); "
                                   "print('é', file=sys.stderr); sys.exit(3)"],
            timeout=30, env=dict(os.environ, X="1"), cwd=tempfile.gettempdir())
        self.assertEqual(rc, 3)
        self.assertEqual(out.split(), ["True", "é"])
        self.assertGreaterEqual(elapsed, 0)

    def test_a_timeout_kills_the_group_and_returns_none(self):
        p = FakePopen([subprocess.TimeoutExpired("t", 1), b"partial \xff"])
        with mock.patch.object(mutate.subprocess, "Popen", return_value=p) as popen, \
                mock.patch.object(mutate.os, "killpg") as killpg:
            rc, out, _ = mutate.run(["t"], 1)
        self.assertIsNone(rc)
        self.assertEqual(out, "partial �")
        killpg.assert_called_once_with(4242, signal.SIGKILL)
        self.assertTrue(popen.call_args.kwargs["start_new_session"])

    def test_a_stop_kills_the_group_waits_and_reraises(self):
        p = FakePopen([KeyboardInterrupt()])
        with mock.patch.object(mutate.subprocess, "Popen", return_value=p), \
                mock.patch.object(mutate.os, "killpg") as killpg, \
                self.assertRaises(KeyboardInterrupt):
            mutate.run(["t"], 1)
        killpg.assert_called_once_with(4242, signal.SIGKILL)
        self.assertEqual(p.waited, 1)

    def test_a_group_already_gone_is_not_an_error(self):
        with mock.patch.object(mutate.os, "killpg", side_effect=ProcessLookupError):
            mutate.kill_group(FakePopen([]))


class ScriptTest(Workspace):
    """The script as `python3 tools/mutate.py` runs it: its __main__ block, in this process so
    that what it runs is measured."""

    def run_script(self, *args, popen=None):
        stdout, stderr = io.StringIO(), io.StringIO()
        with mock.patch.object(sys, "argv", [str(SCRIPT), *args]), \
                mock.patch.object(subprocess, "Popen", popen or FakePopen), \
                mock.patch.object(os, "killpg"), \
                contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr), \
                self.assertRaises(SystemExit) as exited:
            runpy.run_path(str(SCRIPT), run_name="__main__")
        return exited.exception.code, stdout.getvalue(), stderr.getvalue()

    def test_list_exits_0(self):
        self.write("f.cpp", SOURCE)
        status, out, _ = self.run_script("--files", "f.cpp", "--list", "--ops", "lcr")
        self.assertEqual(status, 0)
        self.assertEqual(out, "f.cpp:2:lcr:0\t&& -> ||\n")

    def test_a_stop_signal_exits_with_a_message_and_the_source_restored(self):
        src = self.write("f.cpp", SOURCE)
        self.executable("build/t")
        started = []

        def popen(cmd, **_kwargs):
            started.append(cmd)
            if len(started) == 3:   # the first mutant's build
                self.assertNotEqual(src.read_bytes(), SOURCE.encode())
                os.kill(os.getpid(), signal.SIGTERM)   # the handler raises here
            return FakePopen([b""])

        status, _, _ = self.run_script("--files", "f.cpp", "--only", "f.cpp:3:const:0",
                                       "--build-dir", "build", "--target", "t", "--", "build/t",
                                       popen=popen)
        self.assertEqual(status, "stopped by SIGTERM; the source is restored")
        self.assertEqual(src.read_bytes(), SOURCE.encode())
        self.assertEqual(len(started), 4)   # and the final rebuild


if __name__ == "__main__":
    unittest.main()
