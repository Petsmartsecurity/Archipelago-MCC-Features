"""Effect items (ADR-005): released ids are frozen by hand, the option is off by default (ce_golden proves the old pool is
byte-identical), filler is the only thing replaced, traps follow trap_percentage, and the data package only grows."""
import contextlib
import io
import unittest

from BaseClasses import ItemClassification

from .bases import MCCTestBase
from .ce_snapshot import build

# Frozen by hand: id -> (name, classification). Append only; never edit a line.
FROZEN = {
    81001: ("Effect: Medkit", ItemClassification.filler),
    81002: ("Effect: Recharged Shields", ItemClassification.filler),
    81011: ("Effect: Invulnerable (20 s)", ItemClassification.filler),
    81012: ("Effect: Infinite Ammo (30 s)", ItemClassification.filler),
    81013: ("Effect: Moon Gravity (30 s)", ItemClassification.filler),
    81101: ("Trap: Shields Down", ItemClassification.trap),
    81104: ("Trap: Heavy Gravity (20 s)", ItemClassification.trap),
    81105: ("Trap: Slow Motion (15 s)", ItemClassification.trap),
}


def pool(options, seed=1):
    with contextlib.redirect_stdout(io.StringIO()):
        mw = build(options, seed)
    return mw, [i for i in mw.itempool if i.player == 1]


class TestEffectItems(MCCTestBase):
    def test_frozen_ids_names_classes(self):
        table = self.world.item_name_to_id
        for item_id, (name, cls) in FROZEN.items():
            self.assertEqual(table.get(name), item_id, name)
            self.assertEqual(self.world.create_item(name).classification, cls, name)
            self.assertEqual(self.world.create_item(name).code, item_id, name)
        self.assertEqual({k for k in table if k.startswith(("Effect:", "Trap:"))}, {n for n, _ in FROZEN.values()})
        self.assertIn(81002, FROZEN)  # retired but still in the data package
        self.assertTrue(all(81000 <= i < 82000 for i in FROZEN))
        self.assertEqual(len(set(table.values())), len(table), "item ids must be unique")

    def test_default_off_pool_has_no_effects(self):
        self.assertEqual(self.world.options.effect_items.value, 0)
        self.assertEqual(self.world.fill_slot_data()["effect_items"], 0)
        self.assertEqual({i.name for i in self.multiworld.itempool if i.name.startswith(("Effect:", "Trap:"))}, set())
        self.assertEqual(self.world.get_filler_item_name(), "filler")


class TestEffectPool(unittest.TestCase):
    def test_retired_is_never_generated(self):
        seen = set()
        for seed in range(1, 6):
            _, items = pool({"effect_items": True, "kill_milestone_interval": 10}, seed=seed)
            seen |= {i.name for i in items}
        retired = {81002, 81104, 81105}  # 81104 / 81105 dropped by the user (QUESTIONS #15, 2026-11-24)
        for i in retired:
            self.assertNotIn(FROZEN[i][0], seen)
        self.assertEqual(seen & {n for n, _ in FROZEN.values()}, {n for i, (n, _) in FROZEN.items() if i not in retired})

    def test_on_replaces_only_filler(self):
        _, off = pool({})
        mw, on = pool({"effect_items": True})
        self.assertEqual(len(on), len(off))
        keep = lambda items: sorted(i.name for i in items if i.name != "filler" and not i.name.startswith(("Effect:", "Trap:")))
        self.assertEqual(keep(on), keep(off))
        self.assertEqual([i.name for i in on if i.name == "filler"], [])
        self.assertEqual(mw.worlds[1].fill_slot_data()["effect_items"], 1)
        self.assertEqual(len([i for i in on if i.name.startswith(("Effect:", "Trap:"))]), len([i for i in off if i.name == "filler"]))

    def test_trap_percentage_extremes_and_rate(self):
        _, none = pool({"effect_items": True, "trap_percentage": 0, "kill_milestone_interval": 10}, seed=7)
        self.assertEqual([i for i in none if i.classification == ItemClassification.trap], [])
        _, allt = pool({"effect_items": True, "trap_percentage": 100, "kill_milestone_interval": 10}, seed=7)
        effects = [i for i in allt if i.name.startswith(("Effect:", "Trap:"))]
        self.assertTrue(effects and all(i.classification == ItemClassification.trap for i in effects))
        n = t = 0
        for seed in range(1, 9):
            _, items = pool({"effect_items": True, "trap_percentage": 30, "kill_milestone_interval": 10}, seed=seed)
            eff = [i for i in items if i.name.startswith(("Effect:", "Trap:"))]
            n += len(eff)
            t += sum(1 for i in eff if i.classification == ItemClassification.trap)
        self.assertGreater(n, 400)
        self.assertAlmostEqual(t / n, 0.30, delta=0.06)

    def test_never_progression_and_ce_off_means_none(self):
        _, items = pool({"effect_items": True, "kill_milestone_interval": 10})
        self.assertTrue(all(i.classification != ItemClassification.progression for i in items if i.name.startswith(("Effect:", "Trap:"))))
        mw, items = pool({"effect_items": True, "ce_enabled": False, "h2_enabled": True})
        self.assertEqual([i for i in items if i.name.startswith(("Effect:", "Trap:"))], [])
        self.assertEqual(mw.worlds[1].fill_slot_data()["effect_items"], 0)
