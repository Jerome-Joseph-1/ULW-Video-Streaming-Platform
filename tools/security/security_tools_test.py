#!/usr/bin/env python3
"""The allowlist checker refuses what would waive too much, check-image-pins.py refuses an
unpinned image and a misused exemption, and trivy-report.py marks a finding allowlisted only
where Trivy would honour the entry (docs/adr/0072)."""
import datetime
import importlib.util
import io
import json
import pathlib
import sys
import tempfile
import unittest
from contextlib import redirect_stderr

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

    def test_the_monorepo_exemption_is_only_for_askedin(self):
        askedin, _, sandbox = self.ok_files()
        own = "git.askedin.com/askedin/askedin-monorepo/video-gateway:master"
        rc, _ = self.run_check(askedin, [self.manifest("c.yaml", own)], sandbox)
        self.assertEqual(rc, 1)
        rc, _ = self.run_check([self.manifest("d.yaml", own)], *self.ok_files()[1:])
        self.assertEqual(rc, 0)

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


if __name__ == "__main__":
    sys.exit(unittest.main())
