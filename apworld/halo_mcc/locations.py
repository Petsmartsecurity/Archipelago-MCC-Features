from BaseClasses import ItemClassification, Location

from .data.halo_ce_location_data import CE_LOCATION_DATA
from .data.halo_ce_kill_data import CE_KILL_DATA
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
        # always present: the data package must not depend on options
        **{name: kill_id for name, (_, _, kill_id) in CE_KILL_DATA.items()},
    }
    return location_map

#create locations and place them in their region
def create_locations(world):
    gameSettings = {
        "CE": False,
    }
    if world.options.ce_enabled:
        gameSettings["CE"] = True
    for level, data in LEVEL_DATA.items():
        if gameSettings[data.game]:
            if level != world.final_mission:
                region = world.get_region(level)
                location = MCCLocation(world.player, f"{level} Complete", data.offset, region)
                region.locations.append(location)

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
        if world.options.killsanity.value >= 1:
            for name, (level, _, kill_id) in CE_KILL_DATA.items():
                region = world.get_region(level)
                region.locations.append(MCCLocation(world.player, name, kill_id, region))