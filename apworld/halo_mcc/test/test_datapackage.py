from .bases import MCCTestBase


class TestDataPackageConsistency(MCCTestBase):
    """Every created location/item must be in the data package under the same name, with the world's game."""

    def test_location_names_in_datapackage(self):
        names = self.world.location_name_to_id
        missing = [loc.name for loc in self.multiworld.get_locations(self.player) if loc.name not in names]
        self.assertEqual(missing, [])

    def test_location_ids_match_datapackage(self):
        for loc in self.multiworld.get_locations(self.player):
            if loc.address is not None:
                self.assertEqual(self.world.location_name_to_id.get(loc.name), loc.address, loc.name)

    def test_items_and_locations_carry_world_game(self):
        games = {loc.game for loc in self.multiworld.get_locations(self.player)}
        games |= {item.game for item in self.multiworld.itempool if item.player == self.player}
        self.assertEqual(games, {self.game})

    def test_filler_item_name(self):
        name = self.world.get_filler_item_name()
        self.assertIn(name, self.world.item_name_to_id)


class TestAllSkulls(MCCTestBase):
    options = {"skullsanity": "all"}


class TestInvertedSkulls(MCCTestBase):
    options = {"skullsanity": "inverted"}


class TestRandomFinalMission(MCCTestBase):
    options = {"ce_final_mission": "random_choice", "skullsanity": "off"}
