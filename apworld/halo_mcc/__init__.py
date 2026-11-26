from worlds.AutoWorld import World
from .webWorld import MCCWeb
from .mcc_options import MCCOptions
from . import regions, locations, items, rules, generate_early, goal
from typing import Any

KILLSANITY_VERSION = 1  # slot_data feature level; the launcher warns about a seed newer than it supports (compat.h)

class MCCWorld(World):
    """
    yipee
    """

    game = "Halo The Master Chief Collection"
    options_dataclass = MCCOptions
    options: MCCOptions
    # Raised from upstream's (1, 2, 0): upstream's launcher sends 1.2.0 and is refused by the server (IncompatibleVersion), because it
    # would connect to a kill seed and never send a kill check. Our launcher sends this version (HaloAP_launcher/compat.h).
    required_client_version = (1, 2, 10)
    web = MCCWeb()

    #connects item names to their ID
    item_name_to_id = items.get_item_name_to_id()
    ##same thing as items but for locations
    location_name_to_id = locations.get_location_name_to_id()
    starting_mission:str
    final_mission:str        # CE final mission, "" when CE is off or its final is not a goal (one_final)
    h2_final_mission:str     # Halo 2 final mission, "" when Halo 2 is off or its final is not a goal (one_final)
    missions:list[str]       # CE non-final missions (every CE mission when CE's final is not a goal: goal_mode one_final)
    h2_missions:list[str]    # Halo 2 non-final missions (likewise)
    goal_game:str            # one_final: "CE" / "H2", the game whose final mission is the goal; "" = per_game
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

    # Universal Tracker (FarrakKilhn): regenerate this slot from its slot_data without the player's YAML
    ut_can_gen_without_yaml = True

    @staticmethod
    def interpret_slot_data(slot_data: dict[str, Any]) -> dict[str, Any]:
        return slot_data   # generate_early reads it back from multiworld.re_gen_passthrough

    def generate_early(self):
        generate_early.generate_early(self)

    def enabled_games(self) -> list[str]:
        """Game codes in LEVEL_DATA (CE, H2) that this slot plays."""
        return [g for g, on in (("CE", self.options.ce_enabled), ("H2", self.options.h2_enabled)) if on]

    def fill_slot_data(self) -> dict[str, Any]:
        data = self._slot_data()
        if self.options.h2_enabled:
            # only with Halo 2 on, so a CE-only seed's slot_data is exactly what it was (ce_golden)
            data["h2_skullsanity"] = self.options.h2_skullsanity.value
            data["h2_legendary_skulls"] = int(bool(self.options.h2_legendary_skulls.value))
            data["h2_skulls_required"] = self.options.h2_skulls_required.value
        return data

    def _slot_data(self) -> dict[str, Any]:
        return {
            "final_mission": self.final_mission,
            # skull items are CE-only: with CE off the launcher must not force skulls nothing can release (QA 8.H2.6 M2)
            "skullsanity": self.options.skullsanity.value if self.options.ce_enabled else 0,
            "kill_milestone_interval": self.options.kill_milestone_interval.value,
            "killsanity_version": KILLSANITY_VERSION,
            "kill_infection_interval": self.options.kill_infection_interval.value,
            "kill_species_checks": int(bool(self.options.kill_species_checks.value)),
            "ce_enabled": int(bool(self.options.ce_enabled.value)),
            "h2_enabled": int(bool(self.options.h2_enabled.value)),
            "h2_final_mission": self.h2_final_mission,
            "effect_items": int(items.effects_enabled(self)),
            # goal_mode (PLAN 6.G): 0 per_game, 1 one_final. A seed without the key is per_game. one_final: goal_game is the
            # game whose final mission wins ("CE" / "H2"); the other game's final_mission key is "" (it is an ordinary mission).
            "goal_mode": self.options.goal_mode.value,
            "goal_game": self.goal_game,
            # Universal Tracker (every option / draw the rules depend on; append-only)
            "starting_mission": self.starting_mission,
            "kill_milestone_max_percent": self.options.kill_milestone_max_percent.value,
            "kill_milestone_progression_percent": self.options.kill_milestone_progression_percent.value,
            "kill_difficulty": self.options.kill_difficulty.value,
            "skulls_required": self.options.skulls_required.value,
            "trap_percentage": self.options.trap_percentage.value,
        }