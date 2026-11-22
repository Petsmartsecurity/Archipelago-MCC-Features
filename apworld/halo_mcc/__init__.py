from worlds.AutoWorld import World
from .webWorld import MCCWeb
from .mcc_options import MCCOptions
from . import regions, locations, items, rules, generate_early, goal
from typing import Any

class MCCWorld(World):
    """
    yipee
    """

    game = "Halo The Master Chief Collection"
    options_dataclass = MCCOptions
    options: MCCOptions
    required_client_version = (1, 2, 0)
    web = MCCWeb()

    #connects item names to their ID
    item_name_to_id = items.get_item_name_to_id()
    ##same thing as items but for locations
    location_name_to_id = locations.get_location_name_to_id()
    starting_mission:str
    final_mission:str        # CE final mission, "" when CE is off
    h2_final_mission:str     # Halo 2 final mission, "" when Halo 2 is off
    missions:list[str]       # CE non-final missions
    h2_missions:list[str]    # Halo 2 non-final missions
    ceskulls: list[str] = []

    #creates all logic for locations, entrances, and goal
    def set_rules(self):
        rules.set_rules(self)
        goal.set_goal(self)

    #create regions and place locations inside the regions
    def create_regions(self):
        regions.create_regions(self)
        locations.create_locations(self)

    #creates all items
    def create_items(self):
        items.create_items(self)

    #creates one item with needed information
    def create_item(self, name: str):
        return items.create_item_with_data(self, name)

    #get the name of a filler item, used to fill unfilled locations
    def get_filler_item_name(self):
        return items.get_filler_item_name(self)

    def generate_early(self):
        generate_early.generate_early(self)

    def enabled_games(self) -> list[str]:
        """Game codes in LEVEL_DATA (CE, H2) that this slot plays."""
        return [g for g, on in (("CE", self.options.ce_enabled), ("H2", self.options.h2_enabled)) if on]

    def fill_slot_data(self) -> dict[str, Any]:
        return {
            "final_mission": self.final_mission,
            # skull items are CE-only: with CE off the launcher must not force skulls nothing can release (QA 8.H2.6 M2)
            "skullsanity": self.options.skullsanity.value if self.options.ce_enabled else 0,
            "kill_milestone_interval": self.options.kill_milestone_interval.value,
            "kill_species_checks": int(bool(self.options.kill_species_checks.value)),
            "ce_enabled": int(bool(self.options.ce_enabled.value)),
            "h2_enabled": int(bool(self.options.h2_enabled.value)),
            "h2_final_mission": self.h2_final_mission,
        }