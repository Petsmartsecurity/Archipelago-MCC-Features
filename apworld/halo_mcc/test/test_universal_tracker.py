"""Universal Tracker contract: a slot regenerated from its slot_data (multiworld.re_gen_passthrough, UT's path) must have the same
locations, regions, rules and item pool as the seed that produced the slot_data. Pattern: upstream worlds/messenger/test/test_universal_tracker.py."""
import contextlib
import io
import json
import os
import unittest
from argparse import Namespace

from BaseClasses import CollectionState, MultiWorld
from test.general import gen_steps
from worlds.AutoWorld import call_all

from .. import MCCWorld
from ..mcc_options import EXPERIMENTAL_ENV

BOTH = {"ce_enabled": True, "h2_enabled": True}
H2 = {"ce_enabled": False, "h2_enabled": True}
COMBOS = [
    ("ce_default", {}),
    ("ce_random_final_skulls_all", {"ce_final_mission": "random_choice", "skullsanity": "all", "skulls_required": 5}),
    ("ce_kills_species", {"kill_milestone_interval": 25, "kill_milestone_max_percent": 50, "kill_species_checks": True,
                          "kill_infection_interval": 10, "kill_difficulty": "legendary", "kill_milestone_progression_percent": 20}),
    ("ce_effects", {"effect_items": True, "trap_percentage": 60, "skullsanity": "inverted"}),
    ("both_per_game", {**BOTH, "ce_final_mission": "random_choice", "h2_final_mission": "random_choice"}),
    ("both_one_final", {**BOTH, "goal_mode": "one_final", "ce_final_mission": "random_choice",
                        "h2_final_mission": "random_choice", "skullsanity": "all", "h2_skullsanity": "all"}),
    ("both_everything", {**BOTH, "skullsanity": "non_scoring", "h2_skullsanity": "all", "h2_legendary_skulls": True,
                         "h2_skulls_required": 4, "kill_milestone_interval": 10, "kill_species_checks": True,
                         "effect_items": True, "kill_difficulty": "heroic"}),
    ("h2_only", {**H2, "h2_final_mission": "random_choice", "h2_skullsanity": "inverted", "goal_mode": "one_final"}),
    ("h2_only_all", {**H2, "h2_skullsanity": "all", "h2_legendary_skulls": True}),
]


def _gen(options, seed, passthrough=None):
    mw = MultiWorld(1)
    mw.game = {1: MCCWorld.game}
    mw.player_name = {1: "Tester1"}
    mw.set_seed(seed)
    args = Namespace()
    for key, option in MCCWorld.options_dataclass.type_hints.items():
        setattr(args, key, {1: option.from_any(options.get(key, option.default))})
    mw.set_options(args)
    mw.state = CollectionState(mw)
    if passthrough is not None:
        mw.re_gen_passthrough = {MCCWorld.game: passthrough}
    with contextlib.redirect_stdout(io.StringIO()):
        for step in gen_steps:
            call_all(mw, step)
    return mw


def _shape(mw):
    pool = sorted(i.name for i in mw.itempool)
    access = [i for i in mw.itempool if i.name.endswith(" Access") or "Skull" in i.name]
    locs = [l for l in mw.get_locations(1)]
    regions = list(mw.get_regions(1))
    states = [CollectionState(mw)]
    for n in (1, 3, 7):                      # a few partial states, deterministic order
        st = CollectionState(mw)
        for i in sorted(access, key=lambda i: i.name)[:n]:
            st.collect(i, True)
        states.append(st)
    st = CollectionState(mw)
    for i in mw.itempool:
        st.collect(i, True)
    states.append(st)
    return {
        "locs": sorted((l.name, l.address, l.parent_region.name, int(l.progress_type)) for l in locs),
        "regions": sorted((r.name, sorted(e.name for e in r.exits)) for r in regions),
        "pool": pool, "pre": sorted(i.name for i in mw.precollected_items[1]),
        "access": [[sorted(l.name for l in locs if l.can_reach(s)), sorted(r.name for r in regions if r.can_reach(s)),
                    mw.has_beaten_game(s)] for s in states],
    }


class TestUniversalTracker(unittest.TestCase):
    def setUp(self):
        os.environ[EXPERIMENTAL_ENV] = "1"

    def test_world_contract(self):
        self.assertTrue(MCCWorld.ut_can_gen_without_yaml)
        data = {"a": 1}
        self.assertIs(MCCWorld.interpret_slot_data(data), data)

    def test_regen_matches(self):
        for name, options in COMBOS:
            for seed in (1, 7, 42):
                with self.subTest(case=name, seed=seed):
                    real = _gen(options, seed)
                    slot = real.worlds[1].fill_slot_data()
                    json.dumps(slot)                                  # the AP server sends it as JSON
                    slot = json.loads(json.dumps(slot))
                    # UT: default options (no YAML) + the passthrough; different seed (UT draws its own), no experimental gate
                    os.environ.pop(EXPERIMENTAL_ENV, None)
                    try:
                        regen = _gen({}, seed + 1000, slot)
                    finally:
                        os.environ[EXPERIMENTAL_ENV] = "1"
                    w, r = real.worlds[1], regen.worlds[1]
                    self.assertEqual((w.starting_mission, w.final_mission, w.h2_final_mission, w.goal_game, w.missions,
                                      w.h2_missions), (r.starting_mission, r.final_mission, r.h2_final_mission, r.goal_game,
                                                       r.missions, r.h2_missions))
                    self.assertEqual(slot, r.fill_slot_data())
                    a, b = _shape(real), _shape(regen)
                    # effect fillers are random draws and UT's seed differs: compare the non-effect pool, and the effect count
                    def split(p):
                        return [n for n in p if not n.startswith(("Effect:", "Trap:"))], sum(n.startswith(("Effect:", "Trap:")) for n in p)
                    self.assertEqual(split(a.pop("pool")), split(b.pop("pool")))
                    self.assertEqual(a, b)

    def test_same_seed_regen_pool_identical(self):
        # with the original seed the RNG stream is untouched, so even the random effect fillers are identical
        for name, options in COMBOS:
            with self.subTest(case=name):
                real = _gen(options, 5)
                regen = _gen({}, 5, json.loads(json.dumps(real.worlds[1].fill_slot_data())))
                self.assertEqual(_shape(real), _shape(regen))

    def test_old_slot_data_still_regens(self):
        # a seed made before the UT keys existed: the missing keys fall back to defaults and nothing raises
        real = _gen({"skullsanity": "all"}, 3)
        slot = {k: v for k, v in real.worlds[1].fill_slot_data().items() if k not in (
            "starting_mission", "kill_milestone_max_percent", "kill_milestone_progression_percent", "kill_difficulty",
            "skulls_required", "trap_percentage")}
        regen = _gen({}, 9, slot)
        self.assertEqual(regen.worlds[1].final_mission, real.worlds[1].final_mission)
