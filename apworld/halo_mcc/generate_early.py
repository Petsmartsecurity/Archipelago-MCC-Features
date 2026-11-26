import os

from Options import OptionError

from .mcc_options import EXPERIMENTAL_ENV, GoalMode

from .data.levels import LEVEL_DATA
from .data.skulls import *

H2_RANDOM = 16  # H2FinalMission.option_random_choice; the other values are the mission's MCC number (2..15)


# slot_data key -> option name, for the options a seed's slot_data carries (Universal Tracker regen, see _apply_passthrough)
PASSTHROUGH_OPTIONS = {
    "skullsanity": "skullsanity", "kill_milestone_interval": "kill_milestone_interval",
    "kill_milestone_max_percent": "kill_milestone_max_percent",
    "kill_milestone_progression_percent": "kill_milestone_progression_percent", "kill_difficulty": "kill_difficulty",
    "kill_infection_interval": "kill_infection_interval", "kill_species_checks": "kill_species_checks",
    "skulls_required": "skulls_required", "ce_enabled": "ce_enabled", "h2_enabled": "h2_enabled",
    "goal_mode": "goal_mode", "effect_items": "effect_items", "trap_percentage": "trap_percentage",
    "h2_skullsanity": "h2_skullsanity", "h2_legendary_skulls": "h2_legendary_skulls", "h2_skulls_required": "h2_skulls_required",
}


def passthrough_slot_data(world):
    """Universal Tracker regenerates a slot with multiworld.re_gen_passthrough[game] = the slot_data of the real seed (and default
    options if no YAML). None outside UT."""
    return getattr(world.multiworld, "re_gen_passthrough", {}).get(world.game) or None


def generate_early(world):
    slot = passthrough_slot_data(world)
    if slot:
        # every option comes from the real seed's slot_data; keys an older seed lacks keep the option default
        for key, name in PASSTHROUGH_OPTIONS.items():
            if key in slot:
                getattr(world.options, name).value = int(slot[key])
    ce = bool(world.options.ce_enabled.value)
    h2 = bool(world.options.h2_enabled.value)
    if h2 and not slot and os.environ.get(EXPERIMENTAL_ENV) != "1":
        raise OptionError(f"{world.player_name}: h2_enabled is experimental (Halo 2 only observes in the game for now) and "
                          f"is not available in release seeds; remove it, or set {EXPERIMENTAL_ENV}=1 to generate a test seed")
    if not (ce or h2):
        raise OptionError(f"{world.player_name}: enable at least one game (ce_enabled or h2_enabled)")

    # "" = that game is off; slot_data and the rules use these
    world.final_mission = ""
    world.h2_final_mission = ""
    world.missions = []
    world.h2_missions = []
    world.goal_game = ""   # "" = per_game; one_final: the game whose final mission is the only goal ("CE" / "H2")
    ce_all: list[str] = []
    h2_all: list[str] = []

    if ce:
        missions = [
            "The Pillar of Autumn",
            "Halo (CE)",
            "The Truth and Reconciliation",
            "The Silent Cartographer",
            "Assault on the Control Room",
            "343 Guilty Spark",
            "The Library",
            "Two Betrayals",
            "Keyes",
            "The Maw",
        ]

        if world.options.ce_final_mission == 11:
            world.final_mission = world.random.choice(missions)
        else:
            index = world.options.ce_final_mission - 1
            world.final_mission = missions[index]
        ce_all = list(missions)
        missions.remove(world.final_mission)
        world.missions = [m for m in missions]

    if h2:
        h2_all = [name for name, data in LEVEL_DATA.items() if data.game == "H2"]
        if world.options.h2_final_mission == H2_RANDOM:
            world.h2_final_mission = world.random.choice(h2_all)
        else:
            world.h2_final_mission = h2_all[world.options.h2_final_mission - 2]
        world.h2_missions = [m for m in h2_all if m != world.h2_final_mission]

    # goal_mode (PLAN 6.G). per_game changes nothing here. one_final: with two games one final is drawn (the only RNG use of
    # this option: a CE-only or H2-only seed draws nothing, so it stays identical to per_game); the other game's final
    # mission becomes an ordinary mission: world.<game>_final_mission "" (no final of that game), the mission listed with the rest.
    if world.options.goal_mode == GoalMode.option_one_final:
        world.goal_game = world.random.choice(["CE", "H2"]) if ce and h2 else ("CE" if ce else "H2")
        if ce and world.goal_game != "CE":
            world.final_mission = ""
            world.missions = ce_all
        if h2 and world.goal_game != "H2":
            world.h2_final_mission = ""
            world.h2_missions = h2_all
    if slot:
        # RNG-derived choices come from the seed. The draws above and below still run (their results are discarded) so the
        # RNG stream, hence the filler draws, is identical to the original generation when UT reuses its seed.
        world.final_mission = slot.get("final_mission", "") if ce else ""
        world.h2_final_mission = slot.get("h2_final_mission", "") if h2 else ""
        world.goal_game = slot.get("goal_game", "")
        world.missions = [m for m in ce_all if m != world.final_mission]
        world.h2_missions = [m for m in h2_all if m != world.h2_final_mission]
    if world.final_mission:
        print("final Mission: " + world.final_mission)  # after goal_mode, which may demote it (QA 6.G minor)

    # every enabled game's non-final missions, CE first: the starting mission is drawn from all of them
    candidates = world.missions + world.h2_missions
    world.starting_mission = world.random.choice(candidates)
    if slot and slot.get("starting_mission"):
        world.starting_mission = slot["starting_mission"]

    if world.options.skullsanity.value >=2 and ce:
        skull_list = []
        for i in GAME_SKULLS["ce"]:
            i = f"{i} Skull"
            if i not in skull_list:
                skull_list.append(i)

        world.ceskulls = [i for i in skull_list]
