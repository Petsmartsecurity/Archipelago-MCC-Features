from BaseClasses import CollectionState
from .bases import MCCTestBase
from ..data.halo_ce_kill_data import CE_KILL_DATA


class TestKillsanityMilestones(MCCTestBase):
    options = {"killsanity": "milestones"}

    def kill_locations(self):
        return [loc for loc in self.multiworld.get_locations(self.player) if loc.name in CE_KILL_DATA]

    def test_all_kill_locations_created_with_catalog_ids(self):
        locs = self.kill_locations()
        self.assertEqual(len(locs), len(CE_KILL_DATA))
        self.assertGreater(len(locs), 0)
        for loc in locs:
            self.assertEqual(loc.address, CE_KILL_DATA[loc.name][2])
            self.assertGreaterEqual(loc.address, 1_000_000_000)
            self.assertEqual(loc.parent_region.name, CE_KILL_DATA[loc.name][0])

    def test_location_count_increase(self):
        # every name in the data package is created except the goal mission's "Complete"
        total = len(self.multiworld.get_locations(self.player))
        self.assertEqual(total, len(self.world.location_name_to_id) - 1)
        non_kill = len([n for n in self.world.location_name_to_id if n not in CE_KILL_DATA]) - 1
        self.assertEqual(total - non_kill, len(CE_KILL_DATA))

    def test_item_count_matches_location_count(self):
        items = [i for i in self.multiworld.itempool if i.player == self.player]
        self.assertEqual(len(items), len(self.multiworld.get_unfilled_locations(self.player)))

    def test_unreachable_when_empty_reachable_with_all(self):
        empty = CollectionState(self.multiworld)
        # the starting mission is precollected, so only check kill locations of other missions
        for loc in self.kill_locations():
            if loc.parent_region.name == self.world.starting_mission:
                continue
            self.assertFalse(loc.can_reach(empty), loc.name)
        full = self.multiworld.get_all_state(False)
        for loc in self.kill_locations():
            self.assertTrue(loc.can_reach(full), loc.name)

    def test_goal_mission_kills_exist_and_reachable(self):
        goal = [l for l in self.kill_locations() if l.parent_region.name == self.world.final_mission]
        self.assertTrue(goal)
        full = self.multiworld.get_all_state(False)
        self.assertTrue(all(l.can_reach(full) for l in goal))

    def test_slot_data(self):
        self.assertEqual(self.world.fill_slot_data()["killsanity"], 1)


class TestKillsanityOff(MCCTestBase):
    def test_no_kill_locations_but_ids_in_datapackage(self):
        names = {loc.name for loc in self.multiworld.get_locations(self.player)}
        self.assertFalse(names & set(CE_KILL_DATA))
        self.assertTrue(set(CE_KILL_DATA) <= set(self.world.location_name_to_id))
        self.assertEqual(self.world.fill_slot_data()["killsanity"], 0)
