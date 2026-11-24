"""Halo 2 skulls (PLAN 8.H2.9b, QUESTIONS #16): locations B+21/22, items 92000 + MCC bit, h2_skullsanity tiers, h2_legendary_skulls.
Everything is behind h2_enabled; a CE-only seed is covered by test_ce_regression (ce_golden)."""
import unittest

from BaseClasses import CollectionState, ItemClassification

from .bases import MCCTestBase
from .test_halo2 import BOTH, H2_ONLY, gen
from ..data.halo2_location_data import H2_MISSIONS
from ..data.halo2_skull_data import H2_SKULLS, H2_SKULL_ITEMS, H2_SKULL_NAMES
from ..data.skulls import NON_SCORING
from ..items import H2_SKULL_NONSCORING, H2_SKULL_SCORING

# FROZEN (released IDs only grow): location id -> name, item id -> name
FROZEN_LOCATIONS = {
    202021: "The Armory - That's Just... Wrong Skull", 203021: 'Cairo Station - Thunderstorm Skull',
    204021: 'Outskirts - IWHBYD Skull', 204022: 'Outskirts - Blind Skull', 205021: 'Metropolis - Catch Skull',
    206021: 'The Arbiter - Grunt Birthday Party Skull', 207021: 'The Oracle - Famine Skull',
    208021: 'Delta Halo - Envy Skull', 209021: 'Regret - Assassins Skull', 210021: 'Sacred Icon - Mythic Skull',
    211021: 'Quarantine Zone - Sputnik Skull', 212021: 'Gravemind - Anger Skull', 213021: 'Uprising - Ghost Skull',
    214021: 'High Charity - Iron Skull', 215021: 'The Great Journey - Black Eye Skull',
}
FROZEN_ITEMS = {
    92000: 'Anger Skull (Halo 2)', 92001: 'Assassins Skull (Halo 2)', 92002: 'Bandana Skull (Halo 2)', 92003: 'Black Eye Skull (Halo 2)',
    92004: 'Blind Skull (Halo 2)', 92005: 'Bonded Pair Skull (Halo 2)', 92006: 'Boom Skull (Halo 2)', 92007: 'Catch Skull (Halo 2)',
    92009: 'Envy Skull (Halo 2)', 92010: 'Eye Patch Skull (Halo 2)', 92011: 'Famine Skull (Halo 2)', 92012: 'Feather Skull (Halo 2)',
    92013: 'Fog Skull (Halo 2)', 92014: 'Foreign Skull (Halo 2)', 92015: 'Ghost Skull (Halo 2)', 92016: 'Grunt Birthday Party Skull (Halo 2)',
    92017: 'Grunt Funeral Skull (Halo 2)', 92018: 'Iron Skull (Halo 2)', 92019: 'IWHBYD Skull (Halo 2)', 92020: 'Jacked Skull (Halo 2)',
    92021: 'Malfunction Skull (Halo 2)', 92022: 'Masterblaster Skull (Halo 2)', 92023: 'Mythic Skull (Halo 2)', 92024: 'Pinata Skull (Halo 2)',
    92025: 'Prophet Birthday Party Skull (Halo 2)', 92026: 'Recession Skull (Halo 2)', 92027: 'Scarab Skull (Halo 2)',
    92028: 'SO...ANGRY... Skull (Halo 2)', 92029: 'Sputnik Skull (Halo 2)', 92030: 'Streaking Skull (Halo 2)', 92031: 'Swarm Skull (Halo 2)',
    92032: "That's Just... Wrong Skull (Halo 2)", 92033: 'They Come Back Skull (Halo 2)', 92034: 'Thunderstorm Skull (Halo 2)',
    92037: 'Acrophobia Skull (Halo 2)',
}
LEGENDARY = {"That's Just... Wrong", 'Thunderstorm', 'IWHBYD', 'Catch', 'Ghost', 'Iron', 'Black Eye'}


def h2_skull_items(names):
    return [n for n in names if n in H2_SKULL_ITEMS]


class TestTables(unittest.TestCase):
    def test_frozen_ids(self):
        self.assertEqual({v[1]: k for k, v in H2_SKULLS.items()}, {k: v for k, v in FROZEN_LOCATIONS.items()})
        self.assertEqual({v[0]: k for k, v in H2_SKULL_ITEMS.items()}, FROZEN_ITEMS)
        self.assertEqual(len(H2_SKULLS), 15)
        self.assertEqual(len(H2_SKULL_ITEMS), 35)

    def test_ids_do_not_collide_and_stay_in_their_block(self):
        for name, (mission, loc_id, _l) in H2_SKULLS.items():
            base = H2_MISSIONS[mission][2]
            self.assertIn(loc_id - base, (21, 22), name)
        for name, (item_id, bit, _n) in H2_SKULL_ITEMS.items():
            self.assertEqual(item_id, 92000 + bit)
            self.assertTrue(name.endswith(" Skull (Halo 2)"))
        self.assertEqual(sum(1 for v in H2_SKULLS.values() if v[2]), 7)
        self.assertEqual({k.split(' - ')[1][:-len(' Skull')] for k, v in H2_SKULLS.items() if v[2]}, LEGENDARY)

    def test_tier_lists_partition_the_catalog(self):
        self.assertEqual(sorted(H2_SKULL_SCORING + H2_SKULL_NONSCORING), sorted(H2_SKULL_ITEMS))
        self.assertEqual(len(H2_SKULL_SCORING), 17)
        for n in H2_SKULL_NAMES:
            self.assertEqual(f"{n} Skull (Halo 2)" in H2_SKULL_NONSCORING, n in NON_SCORING)


class TestDataPackage(MCCTestBase):
    def test_option_independent(self):
        names, items = self.world.location_name_to_id, self.world.item_name_to_id
        for name, (_m, loc_id, _l) in H2_SKULLS.items():
            self.assertEqual(names[name], loc_id)
        for name, (item_id, _b, _n) in H2_SKULL_ITEMS.items():
            self.assertEqual(items[name], item_id)
        self.assertEqual(len(set(items.values())), len(items))
        self.assertEqual(len(set(names.values())), len(names))
        # CE skull disablers (90001..) are untouched and distinct from the Halo 2 items
        self.assertEqual(items["Anger Skull"], 90001)
        self.assertNotEqual(items["Iron Skull"], items["Iron Skull (Halo 2)"])

    def test_item_classification(self):
        for name in H2_SKULL_SCORING:
            self.assertEqual(self.world.create_item(name).classification, ItemClassification.progression)
        for name in H2_SKULL_NONSCORING:
            self.assertEqual(self.world.create_item(name).classification, ItemClassification.useful)


class TestOff(MCCTestBase):
    """h2_skullsanity off (the default): hidden-skull locations of the 8 non-Legendary skulls, no skull items."""
    options = H2_ONLY

    def test_locations_and_items(self):
        locs = {l.name for l in self.multiworld.get_locations(self.player)}
        want = {n for n, (_m, _i, leg) in H2_SKULLS.items() if not leg}
        self.assertEqual({n for n in locs if n in H2_SKULLS}, want)
        self.assertEqual(len(want), 8)
        self.assertFalse(h2_skull_items(i.name for i in self.multiworld.itempool))
        slot = self.world.fill_slot_data()
        self.assertEqual((slot["h2_skullsanity"], slot["h2_legendary_skulls"]), (0, 0))

    def test_skull_location_in_its_mission_region(self):
        for loc in self.multiworld.get_locations(self.player):
            if loc.name in H2_SKULLS:
                self.assertEqual((loc.parent_region.name, loc.address), (H2_SKULLS[loc.name][0], H2_SKULLS[loc.name][1]))


class TestLegendary(MCCTestBase):
    options = {**H2_ONLY, "h2_legendary_skulls": True, "h2_skullsanity": "non_scoring"}

    def test_all_fifteen_locations(self):
        locs = {l.name for l in self.multiworld.get_locations(self.player)}
        self.assertTrue(set(H2_SKULLS) <= locs)
        self.assertEqual(self.world.fill_slot_data()["h2_legendary_skulls"], 1)

    def test_only_non_scoring_items(self):
        got = sorted(h2_skull_items(i.name for i in self.multiworld.itempool))
        self.assertEqual(got, sorted(H2_SKULL_NONSCORING))


class TestTiers(unittest.TestCase):
    def pool(self, tier, extra=None):
        mw = gen({**H2_ONLY, "h2_skullsanity": tier, **(extra or {})}, 11, fill=False)
        return mw, sorted(h2_skull_items(i.name for i in mw.itempool))

    def test_pools(self):
        _, off = self.pool("off")
        self.assertEqual(off, [])
        _, ns = self.pool("non_scoring")
        self.assertEqual(ns, sorted(H2_SKULL_NONSCORING))
        _, allp = self.pool("all")
        self.assertEqual(allp, sorted(H2_SKULL_ITEMS))
        _, inv = self.pool("inverted")
        self.assertEqual(inv, sorted(H2_SKULL_ITEMS))

    def test_all_gates_completion_not_ce(self):
        mw, _ = self.pool("all", {"h2_skulls_required": 5})
        w = mw.worlds[1]
        start = w.starting_mission
        loc = next(l for l in mw.get_locations(1) if l.name == f"{start} Complete")
        empty = CollectionState(mw)
        self.assertFalse(loc.can_reach(empty))
        st = CollectionState(mw)
        have = [i for i in mw.itempool if i.name in H2_SKULL_SCORING][:5]
        for i in have:
            st.collect(i, True)
        self.assertTrue(loc.can_reach(st))
        st = CollectionState(mw)
        for i in have[:4]:
            st.collect(i, True)
        self.assertFalse(loc.can_reach(st))

    def test_non_all_tiers_leave_completion_open(self):
        for tier in ("off", "non_scoring", "inverted"):
            mw, _ = self.pool(tier)
            start = mw.worlds[1].starting_mission
            loc = next(l for l in mw.get_locations(1) if l.name == f"{start} Complete")
            self.assertTrue(loc.can_reach(CollectionState(mw)), tier)


class TestBeatable(unittest.TestCase):
    def test_h2_only_over_seeds(self):
        tiers = ("off", "non_scoring", "all", "inverted")
        for seed in range(1, 25):
            mw = gen({**H2_ONLY, "h2_final_mission": "random_choice", "h2_skullsanity": tiers[seed % 4],
                      "h2_legendary_skulls": bool(seed % 2), "h2_skulls_required": 4 + seed % 10}, seed)
            self.assertTrue(mw.has_beaten_game(mw.get_all_state()), seed)
            self.assertFalse([l for l in mw.get_locations(1) if l.item is None], seed)
            self.assertTrue(mw.can_beat_game(CollectionState(mw)), seed)   # reachable in sphere order, not just with everything

    def test_multi_game_over_seeds(self):
        tiers = ("off", "non_scoring", "all", "inverted")
        for seed in range(1, 21):
            mw = gen({**BOTH, "ce_final_mission": "random_choice", "h2_final_mission": "random_choice",
                      "skullsanity": tiers[seed % 4], "h2_skullsanity": tiers[(seed + 1) % 4],
                      "h2_legendary_skulls": bool(seed % 3 == 0)}, seed)
            self.assertTrue(mw.can_beat_game(CollectionState(mw)), seed)
            ce = [i.name for i in mw.itempool if i.name.endswith(" Skull")]
            h2 = [i.name for i in mw.itempool if i.name.endswith(" Skull (Halo 2)")]
            self.assertEqual(len(ce), len(set(ce)))
            self.assertTrue(set(ce).isdisjoint(h2))

    def test_ce_only_seeds_carry_no_h2_skull_things(self):
        mw = gen({"skullsanity": "all", "h2_skullsanity": "all", "h2_legendary_skulls": True}, 5)   # h2 off: the options do nothing
        names = {l.name for l in mw.get_locations(1)} | {i.name for i in mw.itempool}
        self.assertFalse([n for n in names if "(Halo 2)" in n or n in H2_SKULLS])
        slot = mw.worlds[1].fill_slot_data()
        self.assertNotIn("h2_skullsanity", slot)
        self.assertNotIn("h2_legendary_skulls", slot)

    def test_h2_options_draw_no_random_numbers(self):
        """The same seed with and without the Halo 2 skull options (Halo 2 on) consumes the RNG identically up to fill."""
        a = gen({**H2_ONLY}, 9, fill=False)
        b = gen({**H2_ONLY, "h2_legendary_skulls": True}, 9, fill=False)
        self.assertEqual(a.worlds[1].starting_mission, b.worlds[1].starting_mission)
        self.assertEqual(a.random.random(), b.random.random())


if __name__ == "__main__":
    unittest.main()
