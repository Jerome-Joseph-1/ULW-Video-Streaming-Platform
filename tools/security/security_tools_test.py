#!/usr/bin/env python3
"""The allowlist checker refuses what would waive too much, check-image-pins.py refuses an
unpinned image and a misused exemption, trivy-report.py marks a finding allowlisted only
where Trivy would honour the entry (docs/adr/0072), split-resources.py writes nothing
outside its directory, and tools/pathguard.py refuses a command-line path that resolves outside
the repository and the temporary directories."""
import datetime
import importlib.util
import io
import json
import os
import pathlib
import sys
import tempfile
import unittest
from contextlib import redirect_stderr
from unittest import mock

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent


def load(name: str, path: pathlib.Path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


allowlists = load("check_allowlists", HERE / "check-allowlists.py")
pins = load("check_image_pins", ROOT / "deploy" / "local" / "check-image-pins.py")
report = load("trivy_report", HERE / "trivy-report.py")
split = load("split_resources", HERE / "split-resources.py")
pathguard = load("pathguard", ROOT / "tools" / "pathguard.py")

SOON = (datetime.date.today() + datetime.timedelta(days=30)).isoformat()
PAST = (datetime.date.today() - datetime.timedelta(days=1)).isoformat()
STATEMENT = "An entry's statement, long enough to say why it is waived here."
AVCODEC = "pkg:deb/ubuntu/libavcodec60@6.1.1-3ubuntu5?arch=amd64&distro=ubuntu-24.04&epoch=7"
AVFORMAT = "pkg:deb/ubuntu/libavformat60@6.1.1-3ubuntu5?arch=amd64&distro=ubuntu-24.04&epoch=7"


def vuln_entry(**overrides):
    entry = {"id": "CVE-2024-32230", "purls": [AVCODEC], "statement": STATEMENT,
             "expired_at": SOON}
    entry.update(overrides)
    return {"vulnerabilities": [entry]}


class AllowlistTest(unittest.TestCase):
    def test_a_versioned_purl_passes(self):
        self.assertEqual(allowlists.check_trivy(vuln_entry()), [])

    def test_a_purl_without_a_version_fails(self):
        errors = allowlists.check_trivy(vuln_entry(purls=["pkg:deb/ubuntu/libavcodec60"]))
        self.assertTrue(any("one package at one version" in e for e in errors), errors)

    def test_a_wildcard_fails(self):
        for purl in ("pkg:deb/ubuntu/libavcodec60@*", "pkg:deb/ubuntu/*@6.1.1-3ubuntu5",
                     "pkg:deb/ubuntu/libavcodec60@6.1.1?arch=*"):
            with self.subTest(purl=purl):
                self.assertTrue(allowlists.check_trivy(vuln_entry(purls=[purl])))

    def test_not_a_purl_fails(self):
        self.assertTrue(allowlists.check_trivy(vuln_entry(purls=["libavcodec60@6.1.1"])))

    def test_a_vulnerability_needs_purls_not_paths(self):
        self.assertTrue(allowlists.check_trivy(vuln_entry(purls=[], paths=["x"])))
        self.assertTrue(allowlists.check_trivy(vuln_entry(paths=["x"])))

    def test_a_vulnerability_expires_within_92_days(self):
        far = (datetime.date.today() + datetime.timedelta(days=120)).isoformat()
        self.assertTrue(allowlists.check_trivy(vuln_entry(expired_at=far)))

    def test_a_misconfiguration_needs_a_scope(self):
        entry = {"id": "KSV-0001", "statement": STATEMENT, "expired_at": SOON}
        self.assertTrue(allowlists.check_trivy({"misconfigurations": [entry]}))
        entry["paths"] = ["deploy/x.yaml"]
        self.assertEqual(allowlists.check_trivy({"misconfigurations": [entry]}), [])

    def test_the_repository_allowlists_pass(self):
        with redirect_stderr(io.StringIO()) as err:
            self.assertEqual(allowlists.main(), 0, err.getvalue())


DIGEST = "@sha256:" + "a" * 64


class ImagePinsTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        self.images_sh = self.write("images.sh",
                                    "kr_image=docker.io/kube-router:v2\n"
                                    f"kr_digest=sha256:{'b' * 64}\n"
                                    "loose_image=docker.io/loose:v1\n")

    def write(self, name: str, text: str) -> pathlib.Path:
        path = pathlib.Path(self.dir.name) / name
        path.write_text(text, encoding="utf-8")
        return path

    def manifest(self, name: str, image: str) -> pathlib.Path:
        return self.write(name, f"spec:\n  containers:\n    - image: {image}\n")

    def run_check(self, askedin, host, sandbox) -> tuple[int, str]:
        argv = ["--images-sh", str(self.images_sh), "--askedin", *map(str, askedin),
                "--host", *map(str, host), "--sandbox", *map(str, sandbox)]
        err = io.StringIO()
        with redirect_stderr(err):
            try:
                rc = pins.main(argv)
            except SystemExit as e:
                rc = e.code
        return rc, err.getvalue()

    def ok_files(self):
        return ([self.manifest("a.yaml", "docker.io/x/y:1" + DIGEST)],
                [self.manifest("compose.yaml", "ulw/video-gateway:e2e")],
                [self.manifest("sandbox.yaml", "docker.io/kube-router:v2")])

    def test_pinned_files_pass(self):
        self.assertEqual(self.run_check(*self.ok_files()), (0, ""))

    def test_an_unpinned_askedin_image_fails(self):
        _, host, sandbox = self.ok_files()
        rc, err = self.run_check([self.manifest("b.yaml", "docker.io/x/y:1")], host, sandbox)
        self.assertEqual(rc, 1)
        self.assertIn("not pinned by digest", err)

    def test_the_sandbox_build_exemption_is_not_for_askedin(self):
        _, host, sandbox = self.ok_files()
        rc, _ = self.run_check([self.manifest("b.yaml", "ulw/video-gateway:e2e")], host, sandbox)
        self.assertEqual(rc, 1)

    def test_the_own_build_exemption_is_only_for_askedin(self):
        askedin, _, sandbox = self.ok_files()
        own = "ghcr.io/jerome-joseph-1/ulw-video-gateway:main"
        rc, _ = self.run_check(askedin, [self.manifest("c.yaml", own)], sandbox)
        self.assertEqual(rc, 1)
        rc, _ = self.run_check([self.manifest("d.yaml", own)], *self.ok_files()[1:])
        self.assertEqual(rc, 0)

    def test_another_ghcr_image_needs_a_digest(self):
        _, host, sandbox = self.ok_files()
        for image in ("ghcr.io/someone-else/ulw-video-gateway:main", "ghcr.io/jerome-joseph-1/x:1"):
            rc, _ = self.run_check([self.manifest("e.yaml", image)], host, sandbox)
            self.assertEqual(rc, 1, image)

    def test_a_sandbox_tag_needs_a_pinned_digest(self):
        askedin, host, _ = self.ok_files()
        rc, err = self.run_check(askedin, host, [self.manifest("s.yaml", "docker.io/loose:v1")])
        self.assertEqual(rc, 1)
        self.assertIn("pins no digest", err)

    def test_an_unknown_option_is_an_error(self):
        askedin, host, sandbox = self.ok_files()
        err = io.StringIO()
        with redirect_stderr(err), self.assertRaises(SystemExit) as raised:
            pins.main(["--images-sh", str(self.images_sh), "--askedin", str(askedin[0]),
                       "--host", str(host[0]), "--sandbox", str(sandbox[0]), "--loaded", "x"])
        self.assertEqual(raised.exception.code, 2)

    def test_an_empty_file_list_is_an_error(self):
        askedin, host, sandbox = self.ok_files()
        self.assertEqual(self.run_check([], host, sandbox)[0], 2)
        self.assertEqual(self.run_check(askedin, host, [])[0], 2)

    def test_a_missing_file_is_an_error(self):
        _, host, sandbox = self.ok_files()
        self.assertEqual(self.run_check([pathlib.Path("/nonexistent.yaml")], host, sandbox)[0], 2)


def finding(package: str, purl: str) -> dict:
    return {"VulnerabilityID": "CVE-2024-32230", "PkgName": package,
            "PkgIdentifier": {"PURL": purl}, "InstalledVersion": "7:6.1.1-3ubuntu5",
            "Severity": "MEDIUM", "VendorSeverity": {"nvd": 3, "ubuntu": 2},
            "Status": "affected", "Title": "t"}


class ReportTest(unittest.TestCase):
    REPORT = {"ArtifactName": "ulw/video-worker:e2e", "Results": [{"Vulnerabilities": [
        finding("libavcodec60", AVCODEC), finding("libavformat60", AVFORMAT)]}]}

    def summary(self, entries) -> str:
        with tempfile.NamedTemporaryFile("w", suffix=".yaml", delete=False) as f:
            json.dump({"vulnerabilities": entries}, f)  # JSON is YAML
        try:
            return report.summarise(self.REPORT, report.allowlist(pathlib.Path(f.name)))
        finally:
            pathlib.Path(f.name).unlink()

    def entry(self, **overrides):
        return vuln_entry(**overrides)["vulnerabilities"][0]

    def test_an_nvd_high_unfixed_finding_is_listed(self):
        text = self.summary([])
        self.assertIn("CVE-2024-32230", text)
        self.assertIn("no fix (affected)", text)

    def test_an_entry_covers_only_its_packages(self):
        text = self.summary([self.entry()])
        self.assertIn(f"until {SOON} (1 of 2 packages)", text)

    def test_an_entry_for_every_package_covers_the_row(self):
        text = self.summary([self.entry(purls=[AVCODEC, AVFORMAT])])
        self.assertIn(f"until {SOON} |", text)

    def test_an_expired_entry_covers_nothing(self):
        self.assertNotIn("until", self.summary([self.entry(expired_at=PAST)]))

    def test_another_version_or_id_is_not_covered(self):
        other = AVCODEC.replace("3ubuntu5", "3ubuntu6")
        self.assertNotIn("until", self.summary([self.entry(purls=[other])]))
        self.assertNotIn("until", self.summary([self.entry(id="CVE-2024-7055")]))

    def test_purl_matching_follows_trivy(self):
        self.assertTrue(report.purl_covers("pkg:deb/ubuntu/libavcodec60", AVCODEC))
        self.assertTrue(report.purl_covers("pkg:deb/ubuntu/libavcodec60@6.1.1-3ubuntu5?arch=amd64",
                                           AVCODEC))
        self.assertFalse(report.purl_covers("pkg:deb/ubuntu/libavcodec60?arch=arm64", AVCODEC))
        self.assertFalse(report.purl_covers("pkg:deb/ubuntu/libavformat60", AVCODEC))


class SplitResourcesTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.dir.name)
        self.out = self.root / "out"

    def tearDown(self):
        self.dir.cleanup()

    def split(self, manifest: str) -> int:
        source = self.root / "in.yaml"
        source.write_text(manifest, encoding="utf-8")
        with redirect_stderr(io.StringIO()):
            return split.main([str(self.out), str(source)])

    def test_one_file_per_resource(self):
        rc = self.split("kind: ClusterRole\nmetadata:\n  name: system:kube-router\n---\n"
                        "kind: Service\nmetadata:\n  name: api\n  namespace: apps\n")
        self.assertEqual(rc, 0)
        self.assertEqual(sorted(p.name for p in self.out.iterdir()),
                         ["clusterrole-cluster-system:kube-router.yaml", "service-apps-api.yaml"])

    def test_a_name_that_is_not_one_path_component_is_refused(self):
        # "../Escaped" as the kind would write <OUTDIR>/../escaped-x-api.yaml.
        for kind, name in (("../Escaped", "api"), ("Service", "a/b"), ("Service", "a b")):
            with self.subTest(kind=kind, name=name):
                rc = self.split(f"kind: '{kind}'\nmetadata:\n  name: '{name}'\n  namespace: x\n")
                self.assertEqual(rc, 1)
                self.assertEqual(list(self.out.iterdir()), [])
        self.assertEqual(sorted(p.name for p in self.root.iterdir()), ["in.yaml", "out"])


class PathGuardTest(unittest.TestCase):
    """The roots are the repository, $RUNNER_TEMP and the temporary directory; each is set to a
    directory of its own under one scratch directory, so "elsewhere" beside them is outside."""

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        base = pathlib.Path(os.path.realpath(self.dir.name))
        self.repo, self.runner, self.tmp, self.elsewhere = (
            base / "repo", base / "runner", base / "tmp", base / "elsewhere")
        for d in (self.repo, self.runner, self.tmp, self.elsewhere):
            d.mkdir()
        for patch in (mock.patch.object(pathguard, "REPOSITORY", self.repo),
                      mock.patch.dict(os.environ, {"RUNNER_TEMP": str(self.runner)}),
                      mock.patch.object(tempfile, "tempdir", str(self.tmp))):
            patch.start()
            self.addCleanup(patch.stop)

    def refused(self, path) -> str:
        err = io.StringIO()
        with redirect_stderr(err), self.assertRaises(SystemExit) as raised:
            pathguard.inside(path)
        self.assertEqual(raised.exception.code, 2)
        self.assertIn("refused", err.getvalue())
        return err.getvalue()

    def test_paths_inside_the_roots_are_accepted(self):
        for path in (self.repo / "deploy" / "x.yaml", self.repo,
                     self.runner / "trivy-image" / "a.json",
                     self.tmp / "tmp.abc" / "cpp-deps.json", self.repo / "a" / ".." / "b"):
            with self.subTest(path=path):
                self.assertEqual(pathguard.inside(path), pathlib.Path(os.path.realpath(path)))
                self.assertEqual(pathguard.inside(str(path)), pathlib.Path(os.path.realpath(path)))

    def test_a_dot_dot_escape_is_refused(self):
        for path in (self.repo / ".." / "elsewhere" / "x", self.tmp / ".." / "elsewhere",
                     f"{self.runner}/../../elsewhere", self.repo / "..",
                     self.repo / ".." / "repo2"):
            with self.subTest(path=path):
                self.refused(path)

    def test_an_absolute_path_outside_the_roots_is_refused(self):
        for path in (self.elsewhere / "x.json", "/", "/etc/passwd"):
            with self.subTest(path=path):
                self.refused(path)

    def test_a_symlink_inside_a_root_pointing_outside_is_refused(self):
        (self.repo / "link").symlink_to(self.elsewhere, target_is_directory=True)
        (self.tmp / "file-link").symlink_to(self.elsewhere / "f.json")
        self.refused(self.repo / "link")
        self.refused(self.repo / "link" / "out.json")
        self.refused(self.tmp / "file-link")

    def test_a_symlink_within_the_roots_is_accepted(self):
        (self.repo / "link").symlink_to(self.tmp, target_is_directory=True)
        self.assertEqual(pathguard.inside(self.repo / "link" / "x"), self.tmp / "x")

    def test_without_runner_temp_only_the_repository_and_the_temporary_directory(self):
        with mock.patch.dict(os.environ):
            del os.environ["RUNNER_TEMP"]
            self.refused(self.runner / "a.json")
            self.assertEqual(pathguard.inside(self.tmp / "a"), self.tmp / "a")

    def test_split_resources_writes_nothing_outside(self):
        source = self.tmp / "in.yaml"
        source.write_text("kind: Service\nmetadata:\n  name: api\n", encoding="utf-8")
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as raised:
            split.main([str(self.elsewhere / "out"), str(source)])
        self.assertEqual(raised.exception.code, 2)
        self.assertEqual(list(self.elsewhere.iterdir()), [])
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            split.main([str(self.tmp / "out"), str(self.elsewhere / "in.yaml")])
        self.assertFalse((self.tmp / "out").exists())

    def test_trivy_report_reads_nothing_outside(self):
        (self.elsewhere / "r.json").write_text("{}", encoding="utf-8")
        with mock.patch.object(sys, "argv", ["trivy-report.py", str(self.tmp / "ignore.yaml"),
                                             str(self.elsewhere / "r.json")]), \
                redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as raised:
            report.main()
        self.assertEqual(raised.exception.code, 2)


class PathGuardRootsTest(unittest.TestCase):
    def test_the_repository_is_found_from_the_module_and_is_a_root(self):
        self.assertEqual(pathguard.REPOSITORY, ROOT)
        images_sh = ROOT / "deploy" / "local" / "images.sh"
        self.assertEqual(pathguard.inside(images_sh), images_sh)
        self.assertEqual(pathguard.inside(os.path.relpath(images_sh)), images_sh)

    def test_the_temporary_directory_is_a_root(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(pathguard.inside(pathlib.Path(d) / "x"),
                             pathlib.Path(os.path.realpath(d)) / "x")


if __name__ == "__main__":
    sys.exit(unittest.main())
