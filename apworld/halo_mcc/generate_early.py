import os

from Options import OptionError

from .mcc_options import EXPERIMENTAL_ENV

from .data.levels import LEVEL_DATA
from .data.skulls import *

H2_RANDOM = 16  # H2FinalMission.option_random_choice; the other values are the mission's MCC number (2..15)


def generate_early(world):
    ce = bool(world.options.ce_enabled.value)
    h2 = bool(world.options.h2_enabled.value)
    if h2 and os.environ.get(EXPERIMENTAL_ENV) != "1":
        raise OptionError(f"{world.player_name}: h2_enabled is experimental (Halo 2 only observes in the game for now) and "
                          f"is not available in release seeds; remove it, or set {EXPERIMENTAL_ENV}=1 to generate a test seed")
    if not (ce or h2):
        raise OptionError(f"{world.player_name}: enable at least one game (ce_enabled or h2_enabled)")

    # "" = that game is off; slot_data and the rules use these
    world.final_mission = ""
    world.h2_final_mission = ""
    world.missions = []
    world.h2_missions = []
    # every enabled game's non-final missions, CE first: the starting mission is drawn from all of them
    candidates = []

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
        missions.remove(world.final_mission)
        world.missions = [m for m in missions]
        candidates += missions
        print("final Mission: " + world.final_mission)

    if h2:
        h2_all = [name for name, data in LEVEL_DATA.items() if data.game == "H2"]
        if world.options.h2_final_mission == H2_RANDOM:
            world.h2_final_mission = world.random.choice(h2_all)
        else:
            world.h2_final_mission = h2_all[world.options.h2_final_mission - 2]
        world.h2_missions = [m for m in h2_all if m != world.h2_final_mission]
        candidates += world.h2_missions

    world.starting_mission = world.random.choice(candidates)

    if world.options.skullsanity.value >=2 and ce:
        skull_list = []
        for i in GAME_SKULLS["ce"]:
            i = f"{i} Skull"
            if i not in skull_list:
                skull_list.append(i)

        world.ceskulls = [i for i in skull_list]
