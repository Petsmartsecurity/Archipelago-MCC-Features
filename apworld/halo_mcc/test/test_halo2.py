import contextlib
import io
import unittest

from BaseClasses import CollectionState
from Fill import distribute_items_restrictive
from Options import OptionError

from .bases import MCCTestBase
from .ce_snapshot import build
from ..data.halo2_location_data import H2_CHAPTERS, H2_MISSIONS
from ..data.halo2_skull_data import H2_SKULLS
from ..data.levels import LEVEL_DATA
from ..mcc_options import H2FinalMission

CE_NAMES = [n for n, d in LEVEL_DATA.items() if d.game == "CE"]
H2_NAMES = list(H2_MISSIONS)
H2_ONLY = {"ce_enabled": False, "h2_enabled": True}
BOTH = {"ce_enabled": True, "h2_enabled": True}


def gen(options, seed, fill=True):
    """Generate a solo world through pre_fill (and fill), quietly."""
    with contextlib.redirect_stdout(io.StringIO()):
        mw = build(options, seed)
        if fill:
            distribute_items_restrictive(mw)
    return mw


class Common:
    def locs(self):
        return {l.name: l for l in self.multiworld.get_locations(self.player)}

    def pool(self):
        return [i.name for i in self.multiworld.itempool if i.player == self.player]

    def test_pool_matches_locations(self):
        self.assertEqual(len(self.pool()), len(self.multiworld.get_unfilled_locations(self.player)))

    def test_start_is_precollected_and_not_a_final(self):
        pre = [i.name for i in self.multiworld.precollected_items[self.player]]
        self.assertEqual(pre, [f"{self.world.starting_mission} Access"])
        self.assertNotIn(self.world.starting_mission, (self.world.final_mission, self.world.h2_final_mission))
        self.assertNotIn(f"{self.world.starting_mission} Access", self.pool())


class TestH2Only(Common, MCCTestBase):
    options = {**H2_ONLY, "skullsanity": "all", "kill_milestone_interval": 25, "kill_species_checks": True}

    def test_only_h2_content(self):
        locs = self.locs()
        # the 8 non-Legendary hidden skulls are checks (h2_legendary_skulls is off by default)
        want = ({f"{m} Complete" for m in H2_NAMES if m != self.world.h2_final_mission} | set(H2_CHAPTERS)
                | {n for n, v in H2_SKULLS.items() if not v[2]})
        self.assertEqual(set(locs), want)
        self.assertEqual({r.name for r in self.multiworld.get_regions(self.player)}, {"Menu", *H2_NAMES})
        self.assertFalse([n for n in self.pool() if "Skull" in n])
        for n in self.pool():
            self.assertTrue(n == "filler" or n.endswith(" Access"), n)
        want_access = {f"{m} Access" for m in H2_NAMES if m not in (self.world.h2_final_mission, self.world.starting_mission)}
        self.assertEqual({n for n in self.pool() if n != "filler"}, want_access)

    def test_final_and_slot_data(self):
        self.assertEqual(self.world.h2_final_mission, "The Great Journey")
        self.assertEqual(self.world.final_mission, "")
        self.assertIn(self.world.starting_mission, H2_NAMES)
        slot = self.world.fill_slot_data()
        self.assertEqual((slot["ce_enabled"], slot["h2_enabled"], slot["h2_final_mission"]), (0, 1, "The Great Journey"))
        self.assertEqual(slot["final_mission"], "")
        self.assertEqual(slot["skullsanity"], 0)   # no skull items without CE
        self.assertEqual(slot["kill_milestone_interval"], 25)

    def test_locations_in_their_missions(self):
        for name, (mission, loc_id) in H2_CHAPTERS.items():
            loc = self.locs()[name]
            self.assertEqual((loc.parent_region.name, loc.address), (mission, loc_id))

    def test_goal_needs_every_other_access(self):
        final = self.world.h2_final_mission
        access = [i for i in self.multiworld.itempool if i.name.endswith(" Access")]
        state = CollectionState(self.multiworld)
        self.assertFalse(self.multiworld.has_beaten_game(state))
        self.assertFalse(state.can_reach_region(final, self.player))
        for skip in access:
            st = CollectionState(self.multiworld)
            for i in access:
                if i is not skip:
                    st.collect(i, True)
            self.assertFalse(st.can_reach_region(final, self.player), skip.name)
            self.assertFalse(self.multiworld.has_beaten_game(st), skip.name)
        st = CollectionState(self.multiworld)
        for i in access:
            st.collect(i, True)
        self.assertTrue(self.multiworld.has_beaten_game(st))


class TestH2OnlyRandomFinalSeeded(unittest.TestCase):
    def test_many_seeds_fill_and_beat(self):
        finals = set()
        for seed in range(1, 41):
            mw = gen({**H2_ONLY, "h2_final_mission": "random_choice"}, seed)
            w = mw.worlds[1]
            finals.add(w.h2_final_mission)
            self.assertIn(w.h2_final_mission, H2_NAMES)
            self.assertIn(w.starting_mission, [m for m in H2_NAMES if m != w.h2_final_mission])
            self.assertTrue(mw.has_beaten_game(mw.get_all_state()), seed)
            self.assertEqual(w.final_mission, "")
            self.assertFalse([l for l in mw.get_locations(1) if l.item is None])
        self.assertGreater(len(finals), 4)

    def test_each_fixed_final(self):
        for value in range(2, 16):
            name = H2_NAMES[value - 2]
            mw = gen({**H2_ONLY, "h2_final_mission": value}, value)
            self.assertEqual(mw.worlds[1].h2_final_mission, name)
            self.assertNotIn(f"{name} Complete", {l.name for l in mw.get_locations(1)})
            self.assertTrue(mw.has_beaten_game(mw.get_all_state()))


class TestBothGames(Common, MCCTestBase):
    options = {**BOTH, "skullsanity": "all"}

    def test_content(self):
        locs = self.locs()
        w = self.world
        for m in CE_NAMES:
            self.assertEqual(f"{m} Complete" in locs, m != w.final_mission, m)
        for m in H2_NAMES:
            self.assertEqual(f"{m} Complete" in locs, m != w.h2_final_mission, m)
        self.assertTrue(set(H2_CHAPTERS) <= set(locs))
        self.assertTrue([n for n in self.pool() if "Skull" in n])
        self.assertEqual({r.name for r in self.multiworld.get_regions(self.player)}, {"Menu", *CE_NAMES, *H2_NAMES})

    def test_each_game_has_its_own_final(self):
        self.assertEqual(self.world.final_mission, "The Maw")
        self.assertEqual(self.world.h2_final_mission, "The Great Journey")
        self.assertEqual([m for m in self.world.missions], [m for m in CE_NAMES if m != "The Maw"])
        slot = self.world.fill_slot_data()
        self.assertEqual((slot["final_mission"], slot["h2_final_mission"]), ("The Maw", "The Great Journey"))
        self.assertEqual((slot["ce_enabled"], slot["h2_enabled"]), (1, 1))

    def test_goal_needs_both_finals(self):
        w = self.world
        access = [i for i in self.multiworld.itempool if i.name.endswith(" Access")]
        ce = [i for i in access if i.name[:-7] in CE_NAMES]
        h2 = [i for i in access if i.name[:-7] in H2_NAMES]
        skulls = [i for i in self.multiworld.itempool if "Skull" in i.name]
        for part in (ce + skulls, h2):
            st = CollectionState(self.multiworld)
            for i in part:
                st.collect(i, True)
            self.assertFalse(self.multiworld.has_beaten_game(st))
        st = CollectionState(self.multiworld)
        for i in ce:
            st.collect(i, True)
        self.assertTrue(st.can_reach_region(w.final_mission, 1))
        self.assertFalse(st.can_reach_region(w.h2_final_mission, 1))
        self.assertFalse(self.multiworld.has_beaten_game(st))
        st = CollectionState(self.multiworld)
        for i in h2:
            st.collect(i, True)
        self.assertTrue(st.can_reach_region(w.h2_final_mission, 1))
        self.assertFalse(self.multiworld.has_beaten_game(st))
        for i in ce:
            st.collect(i, True)
        self.assertTrue(self.multiworld.has_beaten_game(st))

    def test_skull_rules_stay_ce_only(self):
        # skullsanity all gates CE Complete locations (skulls_required) but never a Halo 2 one
        empty = CollectionState(self.multiworld)
        h2_start = self.world.starting_mission in H2_NAMES
        loc = self.locs()[f"{self.world.starting_mission} Complete"]
        self.assertEqual(loc.can_reach(empty), h2_start)


class TestBothRandomSeeded(unittest.TestCase):
    def test_many_seeds_fill_and_beat(self):
        starts = set()
        for seed in range(1, 31):
            mw = gen({**BOTH, "ce_final_mission": "random_choice", "h2_final_mission": "random_choice",
                      "skullsanity": ("off", "non_scoring", "all", "inverted")[seed % 4]}, seed)
            w = mw.worlds[1]
            self.assertIn(w.final_mission, CE_NAMES)
            self.assertIn(w.h2_final_mission, H2_NAMES)
            self.assertIn(w.starting_mission,
                          [m for m in CE_NAMES + H2_NAMES if m not in (w.final_mission, w.h2_final_mission)])
            starts.add(w.starting_mission in H2_NAMES)
            self.assertTrue(mw.has_beaten_game(mw.get_all_state()), seed)
            self.assertFalse([l for l in mw.get_locations(1) if l.item is None])
        self.assertEqual(starts, {True, False})


class TestCEOffOptions(unittest.TestCase):
    def test_ce_disabled_is_clean(self):
        """CE off + CE-only options: no CE regions, items, skulls, kills; generation works."""
        mw = gen({**H2_ONLY, "skullsanity": "all", "skulls_required": 5, "kill_milestone_interval": 10,
                  "kill_species_checks": True}, 7)
        names = {l.name for l in mw.get_locations(1)} | {i.name for i in mw.itempool} | {r.name for r in mw.get_regions(1)}
        for m in CE_NAMES:
            self.assertFalse([n for n in names if n.startswith(m)], m)
        # CE skulls/kills are gone; the Halo 2 hidden-skull checks ("<Mission> - <Skull> Skull") are not CE's
        self.assertFalse([n for n in names if ("Skull" in n and n not in H2_SKULLS) or "Kill" in n])

    def test_ce_only_is_default(self):
        mw = gen({}, 3)
        w = mw.worlds[1]
        self.assertEqual((w.h2_final_mission, w.h2_missions), ("", []))
        self.assertFalse([l for l in mw.get_locations(1) if l.name in H2_CHAPTERS])

    def test_no_game_raises(self):
        with self.assertRaises(OptionError):
            gen({"ce_enabled": False, "h2_enabled": False}, 1, fill=False)


class TestDataPackage(MCCTestBase):
    def test_option_independent_ids(self):
        names = self.world.location_name_to_id
        items = self.world.item_name_to_id
        self.assertEqual(len(set(names.values())), len(names))
        self.assertEqual(len(set(items.values())), len(items))
        for name, (n, code, base) in H2_MISSIONS.items():
            self.assertEqual(base, 200000 + 1000 * n)
            self.assertEqual(items[f"{name} Access"], base)
            self.assertEqual(names[f"{name} Complete"], base)
            self.assertNotRegex(name, "Access|Skull|Complete")
        for name, (mission, loc_id) in H2_CHAPTERS.items():
            base = H2_MISSIONS[mission][2]
            self.assertTrue(base + 11 <= loc_id <= base + 19, name)
            self.assertEqual(names[name], loc_id)
        self.assertEqual(len(H2_MISSIONS), 14)
        self.assertEqual(len(H2_CHAPTERS), 40)
        self.assertNotIn("The Heretic Complete", names)
        h2_ids = [v for k, v in names.items() if k in H2_CHAPTERS or k.split(" Complete")[0] in H2_MISSIONS]
        self.assertTrue(all(200000 < i < 216000 for i in h2_ids))

    def test_created_h2_items_are_progression(self):
        from BaseClasses import ItemClassification
        for m in H2_NAMES:
            self.assertEqual(self.world.create_item(f"{m} Access").classification, ItemClassification.progression)

    def test_h2_final_option_matches_mission_order(self):
        for name, value in H2FinalMission.options.items():
            if name != "random_choice":
                self.assertEqual(H2_NAMES[value - 2].lower().replace(" ", "_"), name)
        self.assertEqual(H2FinalMission.options["random_choice"], 16)
        self.assertEqual(H2FinalMission.default, H2FinalMission.options["the_great_journey"])
