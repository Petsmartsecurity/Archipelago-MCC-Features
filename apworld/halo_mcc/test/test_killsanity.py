from BaseClasses import CollectionState, LocationProgressType
from .bases import MCCTestBase
from ..data.halo_ce_kill_data import CE_KILL_DATA, CE_KILL_COUNTS, CE_KILL_BASIS
from ..data.kill_ids import (basis_for, datapackage_max_k, datapackage_milestone_max_k, infection_id, infection_ks,
                             infection_name, milestone_cap, milestone_id, milestone_ks, milestone_name)

INT32_MAX = 2**31 - 1


def count_for(level, difficulty):
    no, normal, insane = CE_KILL_COUNTS[level]
    return basis_for(difficulty, normal, insane, CE_KILL_BASIS[level][1])


def expected_milestones(interval, percent, difficulty="normal"):
    """{name: id} the options should create."""
    out = {}
    for level, (no, normal, insane) in CE_KILL_COUNTS.items():
        for k in milestone_ks(count_for(level, difficulty), interval, percent):
            out[milestone_name(level, k)] = milestone_id(no, k)
    return out


def expected_infection(interval):
    return {infection_name(level, k): infection_id(no, k)
            for level, (no, _m, swarm) in CE_KILL_BASIS.items() for k in infection_ks(swarm, interval)}


class KillTestBase(MCCTestBase):
    def milestone_locations(self):
        return {l.name: l for l in self.multiworld.get_locations(self.player)
                if l.name.endswith((" Kill", " Kills")) and l.name not in CE_KILL_DATA}

    def infection_locations(self):
        return {l.name: l for l in self.multiworld.get_locations(self.player) if " Infection Form" in l.name}

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
    options = {"kill_milestone_interval": 0, "kill_milestone_max_percent": 80, "kill_difficulty": "legendary",
               "kill_infection_interval": 0}

    def test_none(self):
        self.assertFalse(self.milestone_locations())
        self.assertFalse(self.species_locations())
        self.assertFalse(self.infection_locations())


class TestMilestones25Normal(KillTestBase):
    options = {"kill_milestone_interval": 25, "kill_milestone_max_percent": 50, "kill_difficulty": "normal"}

    def test_exact_multiples_per_mission(self):
        want = expected_milestones(25, 50)
        got = self.milestone_locations()
        self.assertEqual({n: l.address for n, l in got.items()}, want)
        self.assertTrue(want)
        for level in CE_KILL_COUNTS:
            ks = sorted(int(n.rsplit(" - ", 1)[1].split()[0]) for n in got if n.startswith(level + " - "))
            # the cap is a percent of the MEASURED per-run count (kill_basis.json), not the map count
            self.assertEqual(ks, list(range(25, CE_KILL_BASIS[level][1] * 50 // 100 + 1, 25)), level)
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
        self.assertEqual(slot["kill_infection_interval"], 0)

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


class TestMilestonesDefaults80(KillTestBase):
    """kill_milestone_max_percent defaults to 80, progression percent to 50."""
    options = {"kill_milestone_interval": 10}

    def test_defaults(self):
        self.assertEqual(self.world.options.kill_milestone_max_percent.value, 80)
        self.assertEqual(self.world.options.kill_milestone_progression_percent.value, 50)
        self.assertEqual(self.world.options.kill_infection_interval.value, 0)
        self.assertEqual({n: l.address for n, l in self.milestone_locations().items()}, expected_milestones(10, 80))

    def test_never_above_80_percent_of_measured(self):
        for name, loc in self.milestone_locations().items():
            level = name.rsplit(" - ", 1)[0]
            k = int(name.rsplit(" - ", 1)[1].split()[0])
            self.assertLessEqual(k, CE_KILL_BASIS[level][1] * 80 // 100, name)

    def test_progression_percent_excludes_above_50(self):
        for name, loc in self.milestone_locations().items():
            level = name.rsplit(" - ", 1)[0]
            k = int(name.rsplit(" - ", 1)[1].split()[0])
            excluded = loc.progress_type == LocationProgressType.EXCLUDED
            self.assertEqual(excluded, k > CE_KILL_BASIS[level][1] * 50 // 100, name)


class TestProgressionPercentOption(KillTestBase):
    options = {"kill_milestone_interval": 10, "kill_milestone_progression_percent": 20}

    def test_exclusion_follows_option(self):
        seen_excluded = seen_open = False
        for name, loc in self.milestone_locations().items():
            level = name.rsplit(" - ", 1)[0]
            k = int(name.rsplit(" - ", 1)[1].split()[0])
            excluded = loc.progress_type == LocationProgressType.EXCLUDED
            self.assertEqual(excluded, k > CE_KILL_BASIS[level][1] * 20 // 100, name)
            seen_excluded |= excluded
            seen_open |= not excluded
        self.assertTrue(seen_excluded and seen_open)


class TestMilestonesLegendaryProvisional(KillTestBase):
    options = {"kill_milestone_interval": 25, "kill_milestone_max_percent": 50, "kill_difficulty": "legendary"}

    def test_exact_and_never_above_normal(self):
        got = self.milestone_locations()
        self.assertEqual({n: l.address for n, l in got.items()}, expected_milestones(25, 50, "legendary"))
        normal = expected_milestones(25, 50, "normal")
        self.assertLessEqual(set(got), set(normal))  # provisional: never more than the measured Normal basis
        for level, (_, n, l) in CE_KILL_COUNTS.items():
            if l >= n:
                self.assertEqual(count_for(level, "legendary"), CE_KILL_BASIS[level][1])
            else:
                self.assertLess(count_for(level, "legendary"), CE_KILL_BASIS[level][1])


class TestMilestonesHeroicProvisional(KillTestBase):
    options = {"kill_milestone_interval": 25, "kill_difficulty": "heroic"}

    def test_same_as_normal(self):
        self.assertEqual({n: l.address for n, l in self.milestone_locations().items()}, expected_milestones(25, 80))


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
    options = {"kill_milestone_interval": 1, "kill_milestone_max_percent": 80, "kill_difficulty": "normal",
               "kill_species_checks": True, "kill_infection_interval": 1}

    def test_generates_and_matches(self):
        self.assertEqual({n: l.address for n, l in self.milestone_locations().items()}, expected_milestones(1, 80))
        self.assertIn("The Pillar of Autumn - 1 Kill", self.milestone_locations())
        self.assertNotIn("The Pillar of Autumn - 1 Kills", self.world.location_name_to_id)
        items = [i for i in self.multiworld.itempool if i.player == self.player]
        self.assertEqual(len(items), len(self.multiworld.get_unfilled_locations(self.player)))

    def test_infection_matches_and_is_excluded(self):
        got = self.infection_locations()
        self.assertEqual({n: l.address for n, l in got.items()}, expected_infection(1))
        self.assertTrue(got)
        self.assertTrue(all(l.progress_type == LocationProgressType.EXCLUDED for l in got.values()))
        self.assertIn("The Library - 1 Infection Form", got)
        # only missions with swarm data
        self.assertFalse([n for n in got if n.startswith(("343 Guilty Spark", "The Pillar of Autumn"))])


class TestInterval100(KillTestBase):
    options = {"kill_milestone_interval": 100, "kill_milestone_max_percent": 80, "kill_difficulty": "heroic"}

    def test_generates_and_matches(self):
        self.assertEqual({n: l.address for n, l in self.milestone_locations().items()}, expected_milestones(100, 80))


class TestInfectionOnly(KillTestBase):
    options = {"kill_infection_interval": 25}

    def test_infection(self):
        self.assertFalse(self.milestone_locations())
        got = self.infection_locations()
        self.assertEqual({n: l.address for n, l in got.items()}, expected_infection(25))
        for level, (_, _, swarm) in CE_KILL_BASIS.items():
            ks = sorted(int(n.rsplit(" - ", 1)[1].split()[0]) for n in got if n.startswith(level + " - "))
            self.assertEqual(ks, list(range(25, swarm * 80 // 100 + 1, 25)) if swarm else [], level)
        self.assertEqual(self.world.fill_slot_data()["kill_infection_interval"], 25)

    def test_item_count(self):
        items = [i for i in self.multiworld.itempool if i.player == self.player]
        self.assertEqual(len(items), len(self.multiworld.get_unfilled_locations(self.player)))


class TestDataPackageIsOptionIndependent(KillTestBase):
    def test_datapackage_contents(self):
        names = self.world.location_name_to_id
        for level, (no, normal, insane) in CE_KILL_COUNTS.items():
            top = datapackage_milestone_max_k(normal, insane, CE_KILL_BASIS[level][1])
            # everything Amendment A published (90% of the larger map count) stays, and 80% of the measured basis too
            self.assertGreaterEqual(top, datapackage_max_k(normal, insane))
            self.assertGreaterEqual(top, CE_KILL_BASIS[level][1] * 80 // 100)
            for k in range(1, top + 1):
                self.assertEqual(names[milestone_name(level, k)], milestone_id(no, k))
            self.assertNotIn(milestone_name(level, top + 1), names)
            swarm = CE_KILL_BASIS[level][2]
            for k in range(1, swarm * 80 // 100 + 1):
                self.assertEqual(names[infection_name(level, k)], infection_id(no, k))
            self.assertNotIn(infection_name(level, swarm * 80 // 100 + 1), names)
        for name, (_, _, kill_id) in CE_KILL_DATA.items():
            self.assertEqual(names[name], kill_id)

    def test_ids_unique_and_int32(self):
        names = self.world.location_name_to_id
        ids = list(names.values())
        self.assertEqual(len(set(ids)), len(ids))
        self.assertLessEqual(max(ids), INT32_MAX)
        self.assertGreater(min(ids), 0)

    def test_every_option_combo_ids_are_in_datapackage(self):
        """Any ID a seed can create (milestone / infection, every interval, percent and difficulty) is in the data package."""
        names = self.world.location_name_to_id
        for difficulty in ("normal", "heroic", "legendary"):
            for interval in (1, 2, 3, 7, 25, 100):
                for percent in (10, 50, 80):
                    for name in expected_milestones(interval, percent, difficulty):
                        self.assertIn(name, names)
        for interval in (1, 3, 100):
            for name in expected_infection(interval):
                self.assertIn(name, names)
