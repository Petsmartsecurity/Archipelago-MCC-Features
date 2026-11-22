from BaseClasses import ItemClassification, Location, LocationProgressType

from .data.halo_ce_location_data import CE_LOCATION_DATA
from .data.halo2_location_data import H2_CHAPTERS
from .data.halo_ce_kill_data import CE_KILL_DATA, CE_KILL_COUNTS
from .data.kill_ids import datapackage_max_k, milestone_cap, milestone_id, milestone_ks, milestone_name

SAFE_PERCENT = 50  # milestones above this percent of a mission's enemies are EXCLUDED (filler only)
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
        # always present: the data package must not depend on options
        # species checks, and every milestone k = 1..max for every mission (the largest any option combination can ask for)
        **{name: kill_id for name, (_, _, kill_id) in CE_KILL_DATA.items()},
        **{milestone_name(level, k): milestone_id(mission_no, k)
           for level, (mission_no, normal, insane) in CE_KILL_COUNTS.items()
           for k in range(1, datapackage_max_k(normal, insane) + 1)},
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
        legendary = world.options.kill_difficulty.value == 1
        for level, (mission_no, normal, insane) in CE_KILL_COUNTS.items():
            region = world.get_region(level)
            # `normal` must be reachable on every difficulty, and some missions (a30, a50) have fewer enemies on
            # Legendary than on Normal, so it uses the smaller count
            count = insane if legendary else min(normal, insane)
            for k in milestone_ks(count, interval, percent):
                loc = MCCLocation(world.player, milestone_name(level, k), milestone_id(mission_no, k), region)
                # Above half the mission's enemies, the counts don't model skipped encounters or squads that never
                # spawn, so those milestones hold no progression until reachability data exists (QA 5.4 M1)
                if k > milestone_cap(count, SAFE_PERCENT):
                    loc.progress_type = LocationProgressType.EXCLUDED
                region.locations.append(loc)
        if world.options.kill_species_checks.value:
            for name, (level, _, kill_id) in CE_KILL_DATA.items():
                region = world.get_region(level)
                region.locations.append(MCCLocation(world.player, name, kill_id, region))