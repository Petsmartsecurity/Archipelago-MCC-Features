"""PLAN 6.G / QUESTIONS #13: goal_mode. per_game (default) = today's apworld logic; one_final = one final mission, drawn from
the enabled games' finals, is the only goal and the other games' finals are ordinary pool missions. With a single game
enabled the two modes are identical. The CE-only default stays byte-identical to ce_golden.json (test_ce_regression)."""
import unittest

from BaseClasses import CollectionState

from .bases import MCCTestBase
from .ce_snapshot import CASES, snapshot
from .test_halo2 import BOTH, CE_NAMES, H2_NAMES, H2_ONLY, gen
from ..data.levels import LEVEL_DATA

GOAL_KEYS = {"goal_mode", "goal_game"}
ONE = {"goal_mode": "one_final"}


def _strip(snap):
    return {**snap, "slot": {k: v for k, v in snap["slot"].items() if k not in GOAL_KEYS}}


class TestSingleGameIdentical(unittest.TestCase):
    def test_ce_only_one_final_equals_per_game(self):
        # every CE-only snapshot case (kills, skulls, effects, random final), several seeds: same pool, locations, logic, RNG, fill
        for name, options in CASES:
            for seed in (1, 4, 123456):
                with self.subTest(case=name, seed=seed):
                    a = snapshot(options, seed)
                    b = snapshot({**options, **ONE}, seed)
                    self.assertEqual((a["slot"]["goal_mode"], a["slot"]["goal_game"]), (0, ""))
                    self.assertEqual((b["slot"]["goal_mode"], b["slot"]["goal_game"]), (1, "CE"))
                    self.assertEqual(_strip(a), _strip(b))

    def test_h2_only_one_final_equals_per_game(self):
        for seed in (1, 2, 3, 9):
            for extra in ({}, {"h2_final_mission": "random_choice"}):
                with self.subTest(seed=seed, extra=extra):
                    a = snapshot({**H2_ONLY, **extra}, seed)
                    b = snapshot({**H2_ONLY, **extra, **ONE}, seed)
                    self.assertEqual(b["slot"]["goal_game"], "H2")
                    self.assertEqual(_strip(a), _strip(b))


class TestPerGameUnchanged(MCCTestBase):
    options = dict(BOTH)

    def test_default_slot_data_and_finals(self):
        slot = self.world.fill_slot_data()
        self.assertEqual((slot["goal_mode"], slot["goal_game"]), (0, ""))
        self.assertEqual((slot["final_mission"], slot["h2_final_mission"]), ("The Maw", "The Great Journey"))
        self.assertEqual(self.world.goal_game, "")

    def test_both_finals_are_locked(self):
        locs = {l.name for l in self.multiworld.get_locations(self.player)}
        self.assertNotIn("The Maw Complete", locs)
        self.assertNotIn("The Great Journey Complete", locs)


class TestOneFinalBoth(unittest.TestCase):
    def test_many_seeds(self):
        goal_games = set()
        for seed in range(1, 41):
            opts = {**BOTH, **ONE, "ce_final_mission": "random_choice" if seed % 2 else "the_maw",
                    "h2_final_mission": "random_choice" if seed % 3 else "the_great_journey",
                    "skullsanity": ("off", "non_scoring", "all", "inverted")[seed % 4]}
            mw = gen(opts, seed)
            w = mw.worlds[1]
            with self.subTest(seed=seed, goal=w.goal_game):
                goal_games.add(w.goal_game)
                self.assertIn(w.goal_game, ("CE", "H2"))
                names = {l.name for l in mw.get_locations(1)}
                pool = [i.name for i in mw.itempool] + [i.name for i in mw.precollected_items[1]]
                if w.goal_game == "CE":
                    goal, other_final = w.final_mission, w.h2_final_mission
                    self.assertIn(goal, CE_NAMES)
                    self.assertEqual((w.h2_final_mission, w.h2_missions), ("", H2_NAMES))
                else:
                    goal, other_final = w.h2_final_mission, w.final_mission
                    self.assertIn(goal, H2_NAMES)
                    self.assertEqual((w.final_mission, w.missions), ("", CE_NAMES))
                self.assertEqual(other_final, "")
                # the goal final is locked (no Complete location, no Access item); every other mission is ordinary
                self.assertNotIn(f"{goal} Complete", names)
                self.assertNotIn(f"{goal} Access", pool)
                for m in CE_NAMES + H2_NAMES:
                    if m != goal:
                        self.assertIn(f"{m} Complete", names, m)
                        self.assertIn(f"{m} Access", pool, m)
                self.assertNotEqual(w.starting_mission, goal)
                slot = w.fill_slot_data()
                self.assertEqual((slot["goal_mode"], slot["goal_game"]), (1, w.goal_game))
                self.assertEqual(slot["final_mission" if w.goal_game == "CE" else "h2_final_mission"], goal)
                self.assertEqual(slot["h2_final_mission" if w.goal_game == "CE" else "final_mission"], "")
                # filled, beatable, accessibility full (everything reachable with all items)
                self.assertFalse([l for l in mw.get_locations(1) if l.item is None])
                allstate = mw.get_all_state()
                self.assertTrue(mw.has_beaten_game(allstate))
                self.assertTrue(all(l.can_reach(allstate) for l in mw.get_locations(1)))
        self.assertEqual(goal_games, {"CE", "H2"})

    def test_only_the_goal_final_wins(self):
        for seed in range(1, 13):
            mw = gen({**BOTH, **ONE, "ce_final_mission": "random_choice", "h2_final_mission": "random_choice"}, seed, fill=False)
            w = mw.worlds[1]
            goal = w.final_mission or w.h2_final_mission
            game_names = CE_NAMES if w.goal_game == "CE" else H2_NAMES
            other_names = H2_NAMES if w.goal_game == "CE" else CE_NAMES
            access = [i for i in mw.itempool if i.name.endswith(" Access")]   # CollectionState already holds the precollected start
            with self.subTest(seed=seed):
                # all of the OTHER game's missions (its former final included) do not win...
                st = CollectionState(mw)
                for i in access:
                    if i.name[:-7] in other_names:
                        st.collect(i, True)
                self.assertFalse(mw.has_beaten_game(st))
                for m in other_names:
                    self.assertTrue(st.can_reach_region(m, 1), m)   # the former final is just reachable like the rest
                # ...the goal game's final needs every other mission of ITS game and nothing of the other game
                st = CollectionState(mw)
                mine = [i for i in access if i.name[:-7] in game_names]
                for i in mine:
                    st.collect(i, True)
                self.assertTrue(mw.has_beaten_game(st))
                self.assertTrue(st.can_reach_region(goal, 1))
                for skip in mine:
                    st = CollectionState(mw)
                    for i in mine:
                        if i is not skip:
                            st.collect(i, True)
                    self.assertFalse(mw.has_beaten_game(st), skip.name)

    def test_rng_only_used_with_two_games(self):
        # a one-game seed draws nothing for the mode (also covered by the snapshot equality); two games draw once
        a = gen({**BOTH, "ce_final_mission": "the_maw"}, 5, fill=False).worlds[1]
        b = gen({**BOTH, **ONE}, 5, fill=False).worlds[1]
        self.assertEqual(a.goal_game, "")
        self.assertIn(b.goal_game, ("CE", "H2"))


class TestPerGameManySeeds(unittest.TestCase):
    def test_both_games_beatable_and_accessible(self):
        for seed in range(1, 21):
            mw = gen({**BOTH, "ce_final_mission": "random_choice", "h2_final_mission": "random_choice"}, seed)
            with self.subTest(seed=seed):
                allstate = mw.get_all_state()
                self.assertTrue(mw.has_beaten_game(allstate))
                self.assertTrue(all(l.can_reach(allstate) for l in mw.get_locations(1)))
                self.assertFalse([l for l in mw.get_locations(1) if l.item is None])


class TestDataPackageUnchangedByMode(MCCTestBase):
    def test_every_level_has_access_item_and_complete_location(self):
        # a final that turns ordinary (one_final) needs no new ID: every mission already has both entries
        for level, data in LEVEL_DATA.items():
            self.assertEqual(self.world.item_name_to_id[f"{level} Access"], data.offset)
            self.assertEqual(self.world.location_name_to_id[f"{level} Complete"], data.offset)
