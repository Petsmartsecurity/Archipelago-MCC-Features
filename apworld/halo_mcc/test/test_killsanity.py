from BaseClasses import CollectionState
from .bases import MCCTestBase
from ..data.halo_ce_kill_data import CE_KILL_DATA, CE_KILL_COUNTS
from ..data.kill_ids import datapackage_max_k, milestone_id, milestone_ks, milestone_name

INT32_MAX = 2**31 - 1


def expected_milestones(interval, percent, legendary):
    """{name: id} the options should create."""
    out = {}
    for level, (no, normal, insane) in CE_KILL_COUNTS.items():
        for k in milestone_ks(insane if legendary else min(normal, insane), interval, percent):
            out[milestone_name(level, k)] = milestone_id(no, k)
    return out


class KillTestBase(MCCTestBase):
    def milestone_locations(self):
        return {l.name: l for l in self.multiworld.get_locations(self.player)
                if l.name.endswith((" Kill", " Kills")) and l.name not in CE_KILL_DATA}

    def species_locations(self):
        return [l for l in self.multiworld.get_locations(self.player) if l.name in CE_KILL_DATA]


class TestKillsOff(KillTestBase):
    def test_no_kill_locations_but_ids_in_datapackage(self):
        self.assertFalse(self.milestone_locations())
        self.assertFalse(self.species_locations())
        self.assertTrue(set(CE_KILL_DATA) <= set(self.world.location_name_to_id))
        slot = self.world.fill_slot_data()
        self.assertEqual(slot["kill_milestone_interval"], 0)
        self.assertEqual(slot["kill_species_checks"], 0)
        self.assertNotIn("killsanity", slot)


class TestKillsOffOtherOptions(KillTestBase):
    options = {"kill_milestone_interval": 0, "kill_milestone_max_percent": 90, "kill_difficulty": "legendary"}

    def test_none(self):
        self.assertFalse(self.milestone_locations())
        self.assertFalse(self.species_locations())


class TestMilestones25Normal(KillTestBase):
    options = {"kill_milestone_interval": 25, "kill_milestone_max_percent": 50, "kill_difficulty": "normal"}

    def test_exact_multiples_per_mission(self):
        want = expected_milestones(25, 50, False)
        got = self.milestone_locations()
        self.assertEqual({n: l.address for n, l in got.items()}, want)
        self.assertTrue(want)
        for level, (no, normal, insane) in CE_KILL_COUNTS.items():
            ks = sorted(int(n.rsplit(" - ", 1)[1].split()[0]) for n in got if n.startswith(level + " - "))
            # `normal` caps by the smaller count so it is reachable on any difficulty (a30/a50: Legendary < Normal)
            self.assertEqual(ks, list(range(25, min(normal, insane) * 50 // 100 + 1, 25)), level)
            for n in got:
                if n.startswith(level + " - "):
                    self.assertEqual(got[n].parent_region.name, level)

    def test_names(self):
        self.assertIn("The Pillar of Autumn - 25 Kills", self.milestone_locations())
        self.assertFalse(self.species_locations())

    def test_slot_data(self):
        slot = self.world.fill_slot_data()
        self.assertEqual(slot["kill_milestone_interval"], 25)
        self.assertEqual(slot["kill_species_checks"], 0)

    def test_item_count_matches_location_count(self):
        items = [i for i in self.multiworld.itempool if i.player == self.player]
        self.assertEqual(len(items), len(self.multiworld.get_unfilled_locations(self.player)))

    def test_unreachable_when_empty_reachable_with_all(self):
        empty = CollectionState(self.multiworld)
        for loc in self.milestone_locations().values():
            if loc.parent_region.name == self.world.starting_mission:
                continue
            self.assertFalse(loc.can_reach(empty), loc.name)
        full = self.multiworld.get_all_state(False)
        for loc in self.milestone_locations().values():
            self.assertTrue(loc.can_reach(full), loc.name)

    def test_goal_mission_has_kills(self):
        goal = [l for l in self.milestone_locations().values() if l.parent_region.name == self.world.final_mission]
        self.assertTrue(goal)


class TestMilestones25Legendary(KillTestBase):
    options = {"kill_milestone_interval": 25, "kill_milestone_max_percent": 50, "kill_difficulty": "legendary"}

    def test_exact_and_per_mission_counts(self):
        got = self.milestone_locations()
        self.assertEqual({n: l.address for n, l in got.items()}, expected_milestones(25, 50, True))
        # where Legendary has at least as many enemies as Normal, it never has fewer milestones
        for level, (_, normal, insane) in CE_KILL_COUNTS.items():
            if insane >= normal:
                self.assertGreaterEqual(len(milestone_ks(insane, 25, 50)), len(milestone_ks(normal, 25, 50)))


class TestSpeciesOnly(KillTestBase):
    options = {"kill_species_checks": True}

    def test_species_only(self):
        self.assertFalse(self.milestone_locations())
        locs = self.species_locations()
        self.assertEqual({l.name for l in locs}, set(CE_KILL_DATA))
        for loc in locs:
            self.assertEqual(loc.address, CE_KILL_DATA[loc.name][2])
            self.assertEqual(loc.parent_region.name, CE_KILL_DATA[loc.name][0])
        self.assertEqual(self.world.fill_slot_data()["kill_species_checks"], 1)


class TestInterval1Max(KillTestBase):
    options = {"kill_milestone_interval": 1, "kill_milestone_max_percent": 90, "kill_difficulty": "legendary",
               "kill_species_checks": True}

    def test_generates_and_matches(self):
        self.assertEqual({n: l.address for n, l in self.milestone_locations().items()},
                         expected_milestones(1, 90, True))
        self.assertIn("The Pillar of Autumn - 1 Kill", self.milestone_locations())
        self.assertNotIn("The Pillar of Autumn - 1 Kills", self.world.location_name_to_id)
        items = [i for i in self.multiworld.itempool if i.player == self.player]
        self.assertEqual(len(items), len(self.multiworld.get_unfilled_locations(self.player)))

    def test_high_milestones_are_excluded(self):
        # QA 5.4 M1: above 50% of the enemy count, milestones get filler only
        from BaseClasses import LocationProgressType
        for level, (_, _, insane) in CE_KILL_COUNTS.items():
            for name, loc in self.milestone_locations().items():
                if not name.startswith(level + " - ") or name[len(level) + 3:].split()[0].isdigit() is False:
                    continue
                k = int(name.rsplit(" - ", 1)[1].split()[0])
                excluded = loc.progress_type == LocationProgressType.EXCLUDED
                self.assertEqual(excluded, k > insane * 50 // 100, name)


class TestInterval100(KillTestBase):
    options = {"kill_milestone_interval": 100, "kill_milestone_max_percent": 90, "kill_difficulty": "legendary"}

    def test_generates_and_matches(self):
        self.assertEqual({n: l.address for n, l in self.milestone_locations().items()},
                         expected_milestones(100, 90, True))


class TestDataPackageIsOptionIndependent(KillTestBase):
    def test_datapackage_contents(self):
        names = self.world.location_name_to_id
        for level, (no, normal, insane) in CE_KILL_COUNTS.items():
            top = datapackage_max_k(normal, insane)
            self.assertGreaterEqual(top, insane * 90 // 100)
            self.assertGreaterEqual(top, normal * 90 // 100)
            for k in range(1, top + 1):
                self.assertEqual(names[milestone_name(level, k)], milestone_id(no, k))
            self.assertNotIn(milestone_name(level, top + 1), names)
        for name, (_, _, kill_id) in CE_KILL_DATA.items():
            self.assertEqual(names[name], kill_id)

    def test_ids_unique_and_int32(self):
        names = self.world.location_name_to_id
        ids = list(names.values())
        self.assertEqual(len(set(ids)), len(ids))
        self.assertLessEqual(max(ids), INT32_MAX)
        self.assertGreater(min(ids), 0)
