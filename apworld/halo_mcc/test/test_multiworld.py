"""PLAN 6.R: Halo MCC in a shared multiworld. Generation + fill with 2-4 slots (Halo MCC mixed with built-in worlds that
ship with Archipelago), many seeds, accessibility full / minimal, every kill option. Every generation must fill all
locations and be beatable; with accessibility full every location is reachable. Local generation only."""
import contextlib
import io
import unittest

from BaseClasses import CollectionState
from Fill import distribute_items_restrictive
from test.general import setup_multiworld
from worlds.AutoWorld import AutoWorldRegister

from .. import MCCWorld

OTHERS = ("ChecksFinder", "APQuest", "Bumper Stickers", "Yacht Dice")

KILL_VARIANTS = {
    "off": {},
    "every_kill": {"kill_milestone_interval": 1, "kill_species_checks": True, "kill_infection_interval": 1},
    "25_default_cap": {"kill_milestone_interval": 25},
    "10_cap10": {"kill_milestone_interval": 10, "kill_milestone_max_percent": 10, "kill_milestone_progression_percent": 50},
    "100_legendary_inf": {"kill_milestone_interval": 100, "kill_difficulty": "legendary", "kill_infection_interval": 40},
    "heroic_prog10": {"kill_milestone_interval": 5, "kill_difficulty": "heroic", "kill_milestone_progression_percent": 10,
                     "kill_infection_interval": 7},
    "skulls_all_kills": {"skullsanity": "all", "kill_milestone_interval": 20, "kill_species_checks": True},
}


def generate(game_names, options, seed):
    worlds = [AutoWorldRegister.world_types[g] for g in game_names]
    with contextlib.redirect_stdout(io.StringIO()):
        mw = setup_multiworld(worlds, seed=seed, options=options)
        distribute_items_restrictive(mw)
    return mw


class MultiworldCase(unittest.TestCase):
    def check(self, game_names, options, seed, full):
        mw = generate(game_names, options, seed)
        label = f"{game_names} seed {seed} options {options}"
        self.assertTrue(all(loc.item for loc in mw.get_locations()), f"unfilled: {label}")
        self.assertTrue(mw.can_beat_game(CollectionState(mw)), f"not beatable: {label}")
        if full:
            state = CollectionState(mw)
            state.sweep_for_advancements()
            unreachable = [l.name for l in mw.get_locations() if not state.can_reach_location(l.name, l.player)
                           and l.progress_type.name != "EXCLUDED"]
            self.assertFalse(unreachable[:5], f"not fully accessible: {label}")
        return mw

    def setUp(self):
        missing = [g for g in OTHERS if g not in AutoWorldRegister.world_types]
        if missing:
            self.skipTest(f"built-in worlds missing: {missing}")


class TestMixedMultiworlds(MultiworldCase):
    def test_two_slots_every_kill_option(self):
        for name, kill in KILL_VARIANTS.items():
            for seed in (1, 2, 3):
                for acc in ("full", "minimal"):
                    with self.subTest(kills=name, seed=seed, accessibility=acc):
                        self.check([MCCWorld.game, "ChecksFinder"],
                                   [{**kill, "accessibility": acc}, {"accessibility": acc}], seed, acc == "full")

    def test_three_and_four_slots(self):
        combos = [
            [MCCWorld.game, "ChecksFinder", "APQuest"],
            [MCCWorld.game, "Bumper Stickers", "Yacht Dice", "ChecksFinder"],
            [MCCWorld.game, "APQuest", "Yacht Dice", "Bumper Stickers"],
        ]
        for combo in combos:
            for name in ("every_kill", "25_default_cap", "heroic_prog10"):
                for seed in range(10, 16):
                    acc = "full" if seed % 2 else "minimal"
                    with self.subTest(combo=combo, kills=name, seed=seed, accessibility=acc):
                        opts = [{**KILL_VARIANTS[name], "accessibility": acc}] + [{"accessibility": acc}] * (len(combo) - 1)
                        self.check(combo, opts, seed, acc == "full")

    def test_two_halo_slots_and_others(self):
        """Two Halo MCC slots with different kill settings plus two other games."""
        for seed in range(20, 26):
            with self.subTest(seed=seed):
                self.check([MCCWorld.game, MCCWorld.game, "ChecksFinder", "APQuest"],
                           [KILL_VARIANTS["every_kill"], KILL_VARIANTS["skulls_all_kills"], {}, {}], seed, True)

    def test_halo_only_two_slots_skulls_logic(self):
        for seed in range(30, 36):
            with self.subTest(seed=seed):
                self.check([MCCWorld.game, MCCWorld.game],
                           [KILL_VARIANTS["skulls_all_kills"], KILL_VARIANTS["100_legendary_inf"]], seed, True)

    def test_many_seeds_default_and_kills(self):
        for seed in range(100, 130):
            kill = list(KILL_VARIANTS.values())[seed % len(KILL_VARIANTS)]
            with self.subTest(seed=seed):
                self.check([MCCWorld.game, "ChecksFinder"], [kill, {}], seed, seed % 2 == 0)


if __name__ == "__main__":
    unittest.main()
