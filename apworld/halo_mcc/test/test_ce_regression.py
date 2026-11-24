"""A CE-only seed (Halo 2 off) must come out exactly as before Halo 2 support: same item pool, locations, logic, slot_data
values for the old keys, RNG consumption and fill. ce_golden.json was captured from the code before the change
(10 option combos x 6 seeds, see ce_snapshot.py); the data package may only grow."""
import contextlib
import io
import json
import pathlib
import unittest

from .bases import MCCTestBase
from .ce_snapshot import snapshot

GOLDEN = json.loads((pathlib.Path(__file__).parent / "ce_golden.json").read_text())
NEW_SLOT_KEYS = {"ce_enabled", "h2_enabled", "h2_final_mission", "kill_infection_interval"}


class TestCEOnlyUnchanged(unittest.TestCase):
    def test_snapshots(self):
        for key, case in GOLDEN["cases"].items():
            with self.subTest(case=key), contextlib.redirect_stdout(io.StringIO()):
                got = snapshot(case["options"], case["seed"])
            want = case["snap"]
            self.assertEqual({k: v for k, v in got["slot"].items() if k not in NEW_SLOT_KEYS},
                             {k: v for k, v in want["slot"].items() if k not in NEW_SLOT_KEYS})
            self.assertEqual(got["slot"]["ce_enabled"], 1)
            self.assertEqual((got["slot"]["h2_enabled"], got["slot"]["h2_final_mission"]), (0, ""))
            got = {k: v for k, v in got.items() if k != "slot"}
            want = {k: v for k, v in want.items() if k != "slot"}
            self.assertEqual(got, want)


class TestDataPackageOnlyGrows(MCCTestBase):
    def test_old_ids_and_names_unchanged(self):
        for table, now in (("items", self.world.item_name_to_id), ("locations", self.world.location_name_to_id)):
            old = GOLDEN["datapackage"][table]
            self.assertEqual({k: now.get(k) for k in old}, old, table)
