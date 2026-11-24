"""PLAN 6.R release gates: Halo 2 is not selectable in a release seed (its DLL side only observes), and the options a
friend can pick stay in range."""
import contextlib
import io
import os
import unittest

from Options import OptionError

from .. import MCCWorld
from ..mcc_options import EXPERIMENTAL_ENV
from .ce_snapshot import build


class TestHalo2Gate(unittest.TestCase):
    def setUp(self):
        self._saved = os.environ.pop(EXPERIMENTAL_ENV, None)

    def tearDown(self):
        if self._saved is None:
            os.environ.pop(EXPERIMENTAL_ENV, None)
        else:
            os.environ[EXPERIMENTAL_ENV] = self._saved

    def gen(self, options):
        with contextlib.redirect_stdout(io.StringIO()):
            return build(options, 1)

    def test_h2_refused_without_env(self):
        with self.assertRaises(OptionError):
            self.gen({"h2_enabled": True})
        with self.assertRaises(OptionError):
            self.gen({"h2_enabled": True, "ce_enabled": False})

    def test_h2_refused_when_env_is_not_1(self):
        os.environ[EXPERIMENTAL_ENV] = "0"
        with self.assertRaises(OptionError):
            self.gen({"h2_enabled": True})

    def test_h2_allowed_with_env(self):
        os.environ[EXPERIMENTAL_ENV] = "1"
        mw = self.gen({"h2_enabled": True})
        self.assertEqual(mw.worlds[1].enabled_games(), ["CE", "H2"])

    def test_ce_only_unaffected(self):
        mw = self.gen({})
        self.assertEqual(mw.worlds[1].enabled_games(), ["CE"])


class TestOptionRanges(unittest.TestCase):
    def test_ranges(self):
        h = MCCWorld.options_dataclass.type_hints
        self.assertEqual((h["kill_milestone_interval"].range_start, h["kill_milestone_interval"].range_end), (0, 100))
        self.assertEqual(h["kill_milestone_max_percent"].range_end, 80)
        self.assertEqual(h["kill_milestone_max_percent"].default, 80)
        self.assertEqual(h["kill_milestone_progression_percent"].default, 50)
        self.assertEqual(h["kill_infection_interval"].default, 0)
        with self.assertRaises(Exception):
            h["kill_milestone_max_percent"].from_any(81)
        with self.assertRaises(Exception):
            h["kill_milestone_interval"].from_any(101)

    def test_h2_options_hidden_from_template_in_release(self):
        from Options import Visibility
        if os.environ.get(EXPERIMENTAL_ENV) == "1":
            self.skipTest("experimental environment")
        h = MCCWorld.options_dataclass.type_hints
        self.assertFalse(Visibility.template in h["h2_enabled"].visibility)


class TestApworldPackage(unittest.TestCase):
    """tools/release_pack.py builds the .apworld the way Archipelago's own packager does; AP must be able to read it."""

    def test_pack_and_read_with_ap(self):
        import importlib.util
        import pathlib
        import tempfile
        from worlds.Files import APWorldContainer, container_version
        root = pathlib.Path(__file__).resolve().parents[4]
        spec = importlib.util.spec_from_file_location("release_pack", root / "tools" / "release_pack.py")
        rp = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(rp)
        self.assertEqual(rp.CONTAINER_VERSION, container_version)
        with tempfile.TemporaryDirectory() as d:
            out = pathlib.Path(d) / "halo_mcc.apworld"
            rp.pack_apworld(pathlib.Path(__file__).resolve().parents[1], out)
            self.assertEqual(rp.check_apworld(out), [])
            container = APWorldContainer(str(out))
            container.read()
            self.assertEqual(container.game, MCCWorld.game)
            self.assertEqual(container.world_version.as_simple_string(), "1.2.10")
            import zipfile
            names = zipfile.ZipFile(out).namelist()
            self.assertIn("halo_mcc/__init__.py", names)
            self.assertIn("halo_mcc/data/halo_ce_kill_data.py", names)
            self.assertFalse([n for n in names if "/test/" in n or n.endswith(".pyc")])


class TestCompatibility(unittest.TestCase):
    """QA M1 / M2."""

    def test_progression_range_is_capped_at_50(self):
        h = MCCWorld.options_dataclass.type_hints["kill_milestone_progression_percent"]
        self.assertEqual((h.range_start, h.range_end, h.default), (10, 50, 50))
        with self.assertRaises(Exception):
            h.from_any(51)
        with self.assertRaises(Exception):
            h.from_any(80)

    def test_progression_never_above_study_safe_cap(self):
        from ..data.halo_ce_kill_data import CE_KILL_BASIS
        from ..data.kill_ids import PROGRESSION_SAFE_CAP, milestone_cap
        self.assertEqual(set(PROGRESSION_SAFE_CAP), set(CE_KILL_BASIS))
        for level, (_, basis, _) in CE_KILL_BASIS.items():
            # the largest allowed progression cut (50 percent of the basis) is already inside the study's safe cap
            self.assertLessEqual(milestone_cap(basis, 50), PROGRESSION_SAFE_CAP[level], level)
            # and the safe cap is 80 percent of something that cannot exceed the measured run
            self.assertLessEqual(PROGRESSION_SAFE_CAP[level], milestone_cap(basis, 80), level)

    def test_safe_cap_binds_when_it_is_lower(self):
        """If a mission's safe cap were below the option's cut, the safe cap decides which milestones may hold progression."""
        from unittest import mock
        from .. import locations
        from BaseClasses import LocationProgressType
        from .ce_snapshot import build
        low = dict(locations.PROGRESSION_SAFE_CAP)
        low["The Pillar of Autumn"] = 20
        with mock.patch.dict(locations.PROGRESSION_SAFE_CAP, low), contextlib.redirect_stdout(io.StringIO()):
            mw = build({"kill_milestone_interval": 5}, 1)
        for loc in mw.get_locations(1):
            if loc.name.startswith("The Pillar of Autumn - ") and loc.name.endswith(" Kills"):
                k = int(loc.name.rsplit(" - ", 1)[1].split()[0])
                self.assertEqual(loc.progress_type == LocationProgressType.EXCLUDED, k > 20, loc.name)

    def test_client_version_and_slot_data(self):
        self.assertEqual(MCCWorld.required_client_version, (1, 2, 10))
        self.assertGreater(MCCWorld.required_client_version, (1, 2, 0))  # upstream's launcher sends 1.2.0: refused
        with contextlib.redirect_stdout(io.StringIO()):
            mw = build({}, 1)
        self.assertEqual(mw.worlds[1].fill_slot_data()["killsanity_version"], 1)
