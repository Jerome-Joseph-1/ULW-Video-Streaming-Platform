#!/usr/bin/env python3
"""chat_soak.py's verdict on synthetic samples: a flat run passes, a leak per unit of work that a
per-hour bound would let through fails, and a run too short to resolve the bounds fails."""
import pathlib
import random
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import chat_soak  # noqa: E402
from chat_soak import NODES, column, coverage, judge  # noqa: E402

CLIENTS = 64
# Per node and hour, about what the soak's default load gives each node.
COMMANDS_PER_H = 90_000
DELIVERIES_PER_H = 1_000_000
UPGRADES_PER_H = 1_500


def samples(minutes=360, leak_per_command=0.0, fd_every_min=None, deliveries=True, seed=1):
    rng = random.Random(seed)
    rows = []
    for m in range(minutes + 1):
        row = {"elapsed_min": float(m)}
        for node in NODES:
            c = column(node)
            commands = COMMANDS_PER_H * m // 60
            # Allocator noise: a few pages either way.
            noise = rng.choice([-2, -1, 0, 0, 1, 2]) * 4096
            leaked = int(leak_per_command * commands)
            row[f"{c}_rss_kb"] = (20 * 1024 * 1024 + noise + leaked) // 1024
            row[f"{c}_fds"] = 30 + rng.randint(-3, 3) + (m // fd_every_min if fd_every_min else 0)
            row[f"{c}_commands"] = commands
            row[f"{c}_deliveries"] = DELIVERIES_PER_H * m // 60 if deliveries else 0
            row[f"{c}_upgrades"] = UPGRADES_PER_H * m // 60
        rows.append(row)
    return rows


class JudgeTest(unittest.TestCase):
    def test_flat_six_hours_pass(self):
        passed, report = judge(samples(), CLIENTS)
        self.assertTrue(passed, "\n".join(report))

    def test_half_a_byte_per_command_fails_though_per_hour_it_is_small(self):
        rows = samples(leak_per_command=0.5)
        # 45 KB an hour: a thirtieth of what would reach the pod's limit in 30 days by the hour.
        passed, report = judge(rows, CLIENTS)
        self.assertFalse(passed)
        self.assertTrue(any("per command" in line and "NOT SHOWN FLAT" in line for line in report),
                        "\n".join(report))

    def test_a_run_too_short_to_resolve_fails(self):
        passed, report = judge(samples(minutes=20), CLIENTS)
        self.assertFalse(passed)
        self.assertIn("too few samples", report[0])

    def test_no_deliveries_in_the_window_is_not_judged_and_fails(self):
        passed, report = judge(samples(deliveries=False), CLIENTS)
        self.assertFalse(passed)
        self.assertTrue(any("per delivery: none in the window" in line for line in report))

    def test_descriptors_rising_past_what_is_in_flight_fail(self):
        # One every 4 minutes: 86 over the window, above the bound of every client at once.
        passed, report = judge(samples(fd_every_min=4), CLIENTS)
        self.assertFalse(passed)
        self.assertTrue(any("fds" in line and "NOT FLAT" in line for line in report))

    def test_descriptor_bound_counts_every_connection_the_soak_can_hold(self):
        self.assertEqual(chat_soak.fd_bound(CLIENTS),
                         CLIENTS + chat_soak.VISITORS_IN_FLIGHT + chat_soak.SLOW_MAX + 3)


class CoverageTest(unittest.TestCase):
    def totals(self):
        t = {"sighups": 1, "history_pages": 3, "presence": 2, "prefill_sends": 7,
             "prefill_frames": len(NODES) * chat_soak.PREFILL_FRAMES}
        t.update({f"upgrade_{s}": 1 for s in (401, 404, 426)})
        t.update({f"error_{r}": 1 for r in ("not_json", "malformed", "bad_room", "bad_id",
                                            "bad_body", "not_joined")})
        return t

    def row(self, **zeroed):
        row = {}
        for node in NODES:
            for name in ("deliveries", "forwards", "reassignments", "fenced", "rate_limited",
                         "deduplicated", "replayed", "lossy_drops", "slow_consumers"):
                row[f"{column(node)}_{name}"] = zeroed.get(name, 5)
        return row

    def features(self, history, presence):
        f = chat_soak.Features()
        f.history, f.presence = history, presence
        return f

    def test_every_path_ran(self):
        ok, report = coverage([self.row()], self.totals(), self.features(True, True))
        self.assertTrue(ok, "\n".join(report))

    def test_no_fenced_write_means_the_owner_changes_proved_nothing(self):
        ok, report = coverage([self.row(fenced=0)], self.totals(), self.features(True, True))
        self.assertFalse(ok)
        self.assertIn("  stale owner writes fenced out: NO", report)

    def test_a_warm_up_without_its_prefill_is_not_a_run_to_judge(self):
        totals = self.totals()
        totals["prefill_frames"] -= 1
        ok, report = coverage([self.row()], totals, self.features(True, True))
        self.assertFalse(ok)
        self.assertIn("  warm-up prefill ran: NO", report)

    def test_absent_commands_are_not_required(self):
        totals = self.totals()
        del totals["history_pages"], totals["presence"]
        ok, report = coverage([self.row()], totals, self.features(False, False))
        self.assertTrue(ok, "\n".join(report))
        ok, _ = coverage([self.row()], totals, self.features(True, False))
        self.assertFalse(ok)


if __name__ == "__main__":
    unittest.main()
