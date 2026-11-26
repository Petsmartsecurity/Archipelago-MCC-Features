"""Deterministic summary of a solo Halo MCC generation, used to prove a CE-only seed is unchanged (ce_golden.json was
captured from the code before Halo 2 support). Only public multiworld state and world.final_mission/starting_mission."""
import hashlib
import json
from argparse import Namespace

from BaseClasses import MultiWorld, CollectionState
from Fill import distribute_items_restrictive
from test.general import gen_steps
from worlds.AutoWorld import call_all

from .. import MCCWorld

# (name, options). Kept CE-only: defaults for h2 options (absent in the capture) must mean "off".
CASES = [
    ("default", {}),
    ("skulls_off", {"skullsanity": "off"}),
    ("skulls_non_scoring", {"skullsanity": "non_scoring"}),
    ("skulls_all", {"skullsanity": "all"}),
    ("skulls_all_req3", {"skullsanity": "all", "skulls_required": 3}),
    ("skulls_inverted", {"skullsanity": "inverted"}),
    ("final_keyes", {"ce_final_mission": "keyes"}),
    ("final_random", {"ce_final_mission": "random_choice", "skullsanity": "off"}),
    ("kills_25", {"kill_milestone_interval": 25, "kill_milestone_max_percent": 50}),
    ("kills_25_max80", {"kill_milestone_interval": 25, "kill_milestone_max_percent": 80}),  # was max 90 (PLAN 6.R: max is 80)
    ("kills_every_kill", {"kill_milestone_interval": 1, "kill_milestone_max_percent": 80, "kill_species_checks": True,
                          "kill_infection_interval": 1}),
    ("kills_prog20_heroic", {"kill_milestone_interval": 10, "kill_milestone_progression_percent": 20,
                             "kill_difficulty": "heroic", "kill_infection_interval": 50}),
    ("kills_legendary_species", {"kill_milestone_interval": 50, "kill_difficulty": "legendary",
                                 "kill_species_checks": True, "skullsanity": "all"}),
    # effect items (ADR-005): added after the CE-only golden was captured; the cases above prove the option's default (off) changes nothing
    ("effects_on", {"effect_items": True}),
    ("effects_traps_only", {"effect_items": True, "trap_percentage": 100, "skullsanity": "all"}),
    ("effects_good_only_kills", {"effect_items": True, "trap_percentage": 0, "kill_milestone_interval": 25}),
]
SEEDS = [1, 2, 3, 4, 5, 123456]


def _h(obj):
    return hashlib.sha256(json.dumps(obj, sort_keys=True).encode()).hexdigest()[:16]


def build(options, seed, steps=gen_steps):
    mw = MultiWorld(1)
    mw.game = {1: MCCWorld.game}
    mw.player_name = {1: "Tester1"}
    mw.set_seed(seed)
    args = Namespace()
    for key, option in MCCWorld.options_dataclass.type_hints.items():
        setattr(args, key, {1: option.from_any(options.get(key, option.default))})
    mw.set_options(args)
    mw.state = CollectionState(mw)
    for step in steps:
        call_all(mw, step)
    return mw


def _probes(mw):
    """Logic probe, independent of fill: reachable non-kill locations/regions for each single item, all items, and all
    items but one Access item (is the goal still reachable?)."""
    pool = list(mw.itempool)
    pool_by_name = {}
    for i in pool:
        pool_by_name.setdefault(i.name, i)
    locs = [l for l in mw.get_locations(1) if " Kill" not in l.name and " Infection Form" not in l.name]
    regions = [r for r in mw.get_regions(1)]

    def reach(items):
        st = CollectionState(mw)
        for i in items:
            st.collect(i, True)
        return [sorted(l.name for l in locs if l.can_reach(st)), sorted(r.name for r in regions if r.can_reach(st)),
                mw.has_beaten_game(st)]
    out = {"empty": reach([])}
    for n, i in sorted(pool_by_name.items()):
        out["one:" + n] = reach([i])
    out["all"] = reach(pool)
    for n, i in sorted(pool_by_name.items()):
        if "Access" in n:
            out["all_but:" + n] = reach([x for x in pool if x.name != n])
    return out


def snapshot(options, seed):
    mw = build(options, seed)
    world = mw.worlds[1]
    items = [i.name for i in mw.itempool]
    pre = [i.name for i in mw.precollected_items[1]]
    locs = [(l.name, l.address, l.parent_region.name, int(l.progress_type)) for l in mw.get_locations(1)]
    probes = _probes(mw)
    rng = mw.random.random()          # same RNG consumption through pre_fill -> same next value
    distribute_items_restrictive(mw)
    placed = sorted((l.name, l.item.name) for l in mw.get_locations(1) if l.item)
    return {
        "final": world.final_mission, "start": world.starting_mission, "missions": list(world.missions),
        "n_items": len(items), "items": _h(items), "precollected": pre,
        "n_locs": len(locs), "locs": _h(locs), "probes": _h(probes), "n_probes": len(probes),
        "rng_next": rng, "placed": _h(placed), "slot": world.fill_slot_data(),
        "beatable": mw.has_beaten_game(mw.get_all_state()),
    }


def golden_json(data):
    """Same JSON as json.dumps(data, indent=1, sort_keys=True), but one case / data-package entry per line, so a
    re-capture diffs by case. json.loads reads it unchanged."""
    dump = lambda v: json.dumps(v, sort_keys=True)
    rows = lambda d: ",\n".join(f"{dump(k)}: {dump(v)}" for k, v in sorted(d.items()))
    dp = data["datapackage"]
    return ('{\n"cases": {\n' + rows(data["cases"]) + '\n},\n"datapackage": {\n"items": {\n' + rows(dp["items"])
            + '\n},\n"locations": {\n' + rows(dp["locations"]) + '\n}\n},\n"source_commit": '
            + dump(data["source_commit"]) + '\n}\n')


def capture():
    """Golden content. Only ever written with the pre-Halo 2 code checked out (fork commit 702b51e), or after an
    Archipelago upgrade with a commit whose CE output is already known to be right:
        cd .toolchain/ap-run; python -m worlds.halo_mcc.test.ce_snapshot <source commit> <ce_golden.json>"""
    cases = {f"{name}/{seed}": {"options": options, "seed": seed, "snap": snapshot(options, seed)}
             for name, options in CASES for seed in SEEDS}
    return {"cases": cases, "datapackage": {"items": MCCWorld.item_name_to_id, "locations": MCCWorld.location_name_to_id}}


if __name__ == "__main__":
    import contextlib
    import io
    import pathlib
    import sys
    with contextlib.redirect_stdout(io.StringIO()):
        data = {"source_commit": sys.argv[1], **capture()}
    pathlib.Path(sys.argv[2]).write_bytes(golden_json(data).encode("utf-8"))
