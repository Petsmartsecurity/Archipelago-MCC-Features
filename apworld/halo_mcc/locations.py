from BaseClasses import ItemClassification, Location, LocationProgressType

from .data.halo_ce_location_data import CE_LOCATION_DATA
from .data.halo2_location_data import H2_CHAPTERS
from .data.halo2_skull_data import H2_SKULLS
from .data.halo_ce_kill_data import CE_KILL_DATA, CE_KILL_COUNTS, CE_KILL_BASIS
from .data.kill_ids import (basis_for, datapackage_milestone_max_k, infection_id, infection_ks, infection_name,
                            milestone_cap, milestone_id, PROGRESSION_SAFE_CAP, milestone_ks, milestone_name, MAX_PERCENT)
from .data.levels import LEVEL_DATA

#create our own location object that gets everything from the base location object and change game
class MCCLocation(Location):
    game = "Halo The Master Chief Collection"

#map all locations to their id
def get_location_name_to_id():
    location_map = {
        **{f"{level} Complete": data.offset for level, data in LEVEL_DATA.items()},
        **{f"{data.level} - {name}": data.id for name, data in CE_LOCATION_DATA.items() if data.type == "Chapter"},
        **{f"{data.level} - {name}": data.id for name, data in CE_LOCATION_DATA.items() if data.type == "Skull"},
        **{name: loc_id for name, (_, loc_id) in H2_CHAPTERS.items()},
        **{name: loc_id for name, (_, loc_id, _l) in H2_SKULLS.items()},
        # always present: the data package must not depend on options
        # species checks, and every milestone k = 1..max for every mission (the largest any option combination can ask for)
        **{name: kill_id for name, (_, _, kill_id) in CE_KILL_DATA.items()},
        **{milestone_name(level, k): milestone_id(mission_no, k)
           for level, (mission_no, normal, insane) in CE_KILL_COUNTS.items()
           for k in range(1, datapackage_milestone_max_k(normal, insane, CE_KILL_BASIS[level][1]) + 1)},
        # infection-form checks (ADR-001 Amendment B): every k any seed can ask for, in missions with swarm data
        **{infection_name(level, k): infection_id(mission_no, k)
           for level, (mission_no, _measured, swarm) in CE_KILL_BASIS.items()
           for k in range(1, milestone_cap(swarm, MAX_PERCENT) + 1)},
    }
    return location_map

#create locations and place them in their region
def create_locations(world):
    gameSettings = {
        "CE": bool(world.options.ce_enabled),
        "H2": bool(world.options.h2_enabled),
    }
    finals = (world.final_mission, world.h2_final_mission)
    for level, data in LEVEL_DATA.items():
        if gameSettings[data.game]:
            if level not in finals:
                region = world.get_region(level)
                location = MCCLocation(world.player, f"{level} Complete", data.offset, region)
                region.locations.append(location)

    if gameSettings["H2"]:
        # the final mission has chapters too, like CE
        for name, (level, loc_id) in H2_CHAPTERS.items():
            region = world.get_region(level)
            region.locations.append(MCCLocation(world.player, name, loc_id, region))
        # hidden skulls (QUESTIONS #16): the 7 Legendary-only ones exist only with h2_legendary_skulls (no RNG here)
        for name, (level, loc_id, legendary_only) in H2_SKULLS.items():
            if legendary_only and not world.options.h2_legendary_skulls:
                continue
            region = world.get_region(level)
            region.locations.append(MCCLocation(world.player, name, loc_id, region))

    if gameSettings["CE"]:
        for location, data in CE_LOCATION_DATA.items():
            if data.type == "Chapter":
                region = world.get_region(data.level)
                location = MCCLocation(world.player, f"{data.level} - {location}" ,data.id, region)
                region.locations.append(location)

            if data.type == "Skull":
                region = world.get_region(data.level)
                location = MCCLocation(world.player, f"{data.level} - {location}", data.id, region)
                region.locations.append(location)

        # kill checks live in their mission's region, so they follow that mission's access
        # (the goal mission has no "Complete" location, but its kill checks are still created)
        interval = world.options.kill_milestone_interval.value
        percent = world.options.kill_milestone_max_percent.value
        progression = world.options.kill_milestone_progression_percent.value
        difficulty = world.options.kill_difficulty.current_key
        infection = world.options.kill_infection_interval.value
        for level, (mission_no, normal, insane) in CE_KILL_COUNTS.items():
            region = world.get_region(level)
            # the measured per-run count of this mission (kill_basis.json); see kill_ids.basis_for for the
            # provisional Heroic / Legendary rules
            count = basis_for(difficulty, normal, insane, CE_KILL_BASIS[level][1])
            for k in milestone_ks(count, interval, percent):
                loc = MCCLocation(world.player, milestone_name(level, k), milestone_id(mission_no, k), region)
                # The per-mission count restarts every run, and a player who skips sections ends below the measured
                # count, so milestones above `progression` percent hold no progression (filler only; QA 5.4 M1)
                if k > min(milestone_cap(count, progression), PROGRESSION_SAFE_CAP[level]):
                    loc.progress_type = LocationProgressType.EXCLUDED
                region.locations.append(loc)
            # Flood infection forms: only missions with swarm data; never progression (the numbers vary with play style)
            for k in infection_ks(CE_KILL_BASIS[level][2], infection):
                loc = MCCLocation(world.player, infection_name(level, k), infection_id(mission_no, k), region)
                loc.progress_type = LocationProgressType.EXCLUDED
                region.locations.append(loc)
        if world.options.kill_species_checks.value:
            for name, (level, _, kill_id) in CE_KILL_DATA.items():
                region = world.get_region(level)
                region.locations.append(MCCLocation(world.player, name, kill_id, region))