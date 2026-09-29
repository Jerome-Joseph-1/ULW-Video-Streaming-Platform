#!/usr/bin/env python3
"""stunner_check.verify() on synthetic probe reports: the acceptance rules hold without a
cluster, so loosening one (dropping the port from the mapped comparison, accepting a success
for a forbidden peer) fails here instead of only against a live STUNner."""
import contextlib
import copy
import io
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import stunner_check  # noqa: E402
from vod_flow import Failure  # noqa: E402

CLIENT = stunner_check.PROBE_ADDRESS
STUNNERD = "10.244.0.7"
LIVEKIT = "10.244.0.8"
FORBIDDEN = {"10.244.0.6": "the operator", "127.0.0.1": "the relay's loopback",
             "172.19.0.2": "the node"}
REFUSED = {"class": "error", "error": [0, ""]}


def good_report():
    return {
        "binding": {"local": [CLIENT, 40000], "mapped": [CLIENT, 40000], "class": "success"},
        "allocate": {"local": [CLIENT, 40001], "class": "success", "integrity": True,
                     "mapped": [CLIENT, 40001], "relayed": [STUNNERD, 50000]},
        "permit": {LIVEKIT: {"class": "success", "integrity": True}},
        "forbid": {peer: dict(REFUSED) for peer in FORBIDDEN},
        "release": {"class": "success", "integrity": True},
        "wrong_password": {"class": "error", "error": [400, ""]},
        "expired": {"class": "error", "error": [401, "Unauthorized"]},
    }


def verify(report):
    with contextlib.redirect_stdout(io.StringIO()):
        stunner_check.verify(report, STUNNERD, LIVEKIT, FORBIDDEN)


class VerifyTest(unittest.TestCase):
    def assert_rejected(self, change, message):
        report = copy.deepcopy(good_report())
        change(report)
        with self.assertRaises(Failure) as failure:
            verify(report)
        self.assertIn(message, str(failure.exception))

    def test_a_report_of_a_working_relay_passes(self):
        verify(good_report())

    def test_a_masqueraded_client_address_fails(self):
        def masquerade(r):
            r["binding"]["mapped"] = ["10.244.0.1", 40000]
        self.assert_rejected(masquerade, "XOR-MAPPED-ADDRESS")

    def test_a_rewritten_port_fails_even_with_the_right_address(self):
        def repoint(r):
            r["binding"]["mapped"] = [CLIENT, 25978]
        self.assert_rejected(repoint, "XOR-MAPPED-ADDRESS")

    def test_a_probe_that_ran_elsewhere_fails(self):
        def elsewhere(r):
            r["binding"]["local"] = r["binding"]["mapped"] = ["172.17.0.9", 40000]
        self.assert_rejected(elsewhere, "not on")

    def test_an_allocation_signed_with_another_key_fails(self):
        def unsigned(r):
            r["allocate"]["integrity"] = False
        self.assert_rejected(unsigned, "not signed")

    def test_a_masqueraded_allocation_fails(self):
        def masquerade(r):
            r["allocate"]["mapped"] = ["10.244.0.1", 40001]
        self.assert_rejected(masquerade, "allocate XOR-MAPPED-ADDRESS")

    def test_a_relay_from_another_address_fails(self):
        def other(r):
            r["allocate"]["relayed"] = ["10.244.0.99", 50000]
        self.assert_rejected(other, "stunnerd")

    def test_a_refused_permission_to_livekit_fails(self):
        def refuse(r):
            r["permit"][LIVEKIT] = dict(REFUSED)
        self.assert_rejected(refuse, "LiveKit")

    def test_a_granted_permission_to_any_forbidden_peer_fails(self):
        for peer in FORBIDDEN:
            def grant(r, peer=peer):
                r["forbid"][peer] = {"class": "success", "integrity": True}
            with self.subTest(peer=peer):
                self.assert_rejected(grant, peer)

    def test_a_forbidden_peer_the_probe_never_tried_fails(self):
        def skip(r):
            del r["forbid"]["127.0.0.1"]
        self.assert_rejected(skip, "did not try")

    def test_a_refusal_for_some_other_reason_fails(self):
        def quota(r):
            r["forbid"]["10.244.0.6"] = {"class": "error", "error": [486, "Allocation Quota"]}
        self.assert_rejected(quota, "the operator")

    def test_an_allocation_with_bad_credentials_that_succeeds_fails(self):
        for case in ("wrong_password", "expired"):
            def accept(r, case=case):
                r[case] = {"class": "success"}
            with self.subTest(case=case):
                self.assert_rejected(accept, case)

    def test_bad_credentials_refused_for_another_reason_fail(self):
        def mismatch(r):
            r["expired"] = {"class": "error", "error": [437, "Allocation Mismatch"]}
        self.assert_rejected(mismatch, "expired")

    def test_an_allocation_that_cannot_be_released_fails(self):
        def stuck(r):
            r["release"] = {"class": "error", "error": [437, ""]}
        self.assert_rejected(stuck, "releasing")


if __name__ == "__main__":
    unittest.main()
