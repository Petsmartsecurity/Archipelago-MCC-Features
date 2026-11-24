from dataclasses import dataclass
import os

from Options import PerGameCommonOptions, Toggle, Choice, Range, Visibility

# Halo 2 is observe-only on the DLL side (ADR-004 decision 5): it cannot be selected in a release seed. Generation fails
# with an OptionError unless HALOAP_EXPERIMENTAL=1 (checked in generate_early.py); the options are hidden from the template.
EXPERIMENTAL_ENV = "HALOAP_EXPERIMENTAL"
_H2_VISIBILITY = Visibility.all if os.environ.get(EXPERIMENTAL_ENV) == "1" else Visibility.spoiler


class SkullSanity(Choice):
    """
    Adds skull disabler items to the item pool. All included skulls are forced
    on at game start; receiving a "Disable X" item lets you toggle that skull off.

    off:         No skull items or logic.
    non_scoring: non scoring skulls are locked off, reciving the item for them lets you turn them on.
    all: All skulls are locked, scoring skulls are locked on and non scoring skulls are locked off. reciving the item for them lets you toggle them.
    inverted: all skulls are locked off, reciving the item for them lets you turn them on.
    """
    display_name = "Skull Sanity"
    option_off = 0
    option_non_scoring = 1
    option_all = 2
    option_inverted = 3
    default = 1


class KillMilestoneInterval(Range):
    """
    Adds a check every N enemy kills in each mission (N, 2N, 3N, ...), so with 25 you get checks at 25, 50, 75
    kills and so on. The count restarts each time you start a mission, and a check is sent the moment the kill
    happens (reverting to a checkpoint does not take it back). Kills by allies and vehicles count; enemies that
    a script removes count too; Flood infection forms do not (see kill_infection_interval).

    0 turns kill milestone checks off. 1 is a check for every single kill up to the cap, which is hundreds per
    mission, so choose small numbers with care: many more locations, and a bigger multiworld. The highest milestone
    in a mission is set by kill_milestone_max_percent, kill_difficulty and the measured enemy count of that mission.
    """
    display_name = "Kill Milestone Interval"
    range_start = 0
    range_end = 100
    default = 0


class KillMilestoneMaxPercent(Range):
    """
    The highest kill milestone in a mission is at most this percent of the number of counted kills a normal
    playthrough of that mission really produces (measured in real runs on Normal, not the number of enemies placed
    on the map, which is larger because many only spawn in some routes), so every milestone can be reached in one
    run. For example 80 on a mission where a run counts 133 kills gives milestones up to 106 kills. Never more
    than 80. Only used when kill_milestone_interval is above 0.
    """
    display_name = "Kill Milestone Max Percent"
    range_start = 10
    range_end = 80
    default = 80


class KillMilestoneProgressionPercent(Range):
    """
    Milestones above this percent of the mission's measured kill count are still checks, but they are EXCLUDED
    from holding progression items: only filler (and other non-progression items) can be placed there. The kill
    count restarts every time a mission is started, and a player who skips sections or plays carefully can end a
    run well below the measured count, so a progression item behind a high milestone could be stranded. The default
    keeps everything above half of a mission's kills filler-only. Lower numbers are safer; raise it only if
    everybody in the multiworld plays missions out in full. Only used when kill_milestone_interval is above 0.
    """
    display_name = "Kill Milestone Progression Percent"
    range_start = 0
    range_end = 80
    default = 50


class KillDifficulty(Choice):
    """
    Which difficulty the milestone cap is calibrated for. Only Normal has been measured (real playthroughs on
    Normal); the other two are PROVISIONAL rules until Heroic / Legendary runs are measured.

    normal:    The measured Normal count. Reachable on any difficulty of the campaign.
    heroic:    Provisional: the same measured Normal count (Heroic has not been measured; it spawns more enemies
               than Normal, so the Normal count is the safe choice).
    legendary: Provisional: the measured Normal count, scaled down by (Legendary / Normal enemies on the map) in the
               two missions where Legendary has fewer enemies (Halo CE, The Truth and Reconciliation), never scaled up. It does
               not yield extra milestones on Legendary until Legendary is measured.
    """
    display_name = "Kill Difficulty"
    option_normal = 0
    option_legendary = 1
    option_heroic = 2
    default = 0


class KillInfectionInterval(Range):
    """
    Adds a check every N Flood infection forms killed in a mission (N, 2N, 3N, ...). Infection forms are the small
    Flood swarm that bursts out of pods and carriers; they are not counted by kill_milestone_interval. A mission only
    has these checks when real playthroughs recorded its infection forms (today: The Library, Two Betrayals, Keyes,
    The Maw; not 343 Guilty Spark). The highest check is at most 80 percent of the infection forms one run produced,
    and the numbers vary a lot with play style, so these checks never hold progression items (filler only).

    0 turns infection form checks off.
    """
    display_name = "Kill Infection Interval"
    range_start = 0
    range_end = 100
    default = 0


class KillSpeciesChecks(Toggle):
    """
    Adds a "First <enemy type> Kill" check in each mission for every enemy type that appears there in numbers
    (for example grunt, elite, jackal). Independent of the kill milestones.
    """
    display_name = "Kill Species Checks"
    default = False


class SkullsRequired(Range):
    """
    How many scoring skulls need to be disabled before mission completion is in logic
    only affects all option for skullsanity
    """
    display_name = "Required Skull Disables"
    range_start = 0
    range_end = 14
    default = 8


# class Powerups(Toggle):
#    """Should Powerups be locations"""
#    display_name = "Enable Powerups"
#    default = True

class CeEnabled(Toggle):
    """Should CeEnabled be locations"""
    display_name = "Enable Halo CE"
    default = True


class CeFinalMission(Choice):
    """What the goal mission for halo CE is"""
    display_name = "Final CE Mission"
    default = 10
    option_pillar_of_autumn = 1
    option_halo = 2
    option_truth_and_reconciliation = 3
    option_silent_cartographer = 4
    option_assault_on_the_control_room = 5
    option_343_guilty_spark = 6
    option_library = 7
    option_two_betrayals = 8
    option_keyes = 9
    option_the_maw = 10
    option_random_choice = 11


class H2Enabled(Toggle):
    """EXPERIMENTAL, not available in release seeds: Halo 2 support only observes in the game for now, so generation
    refuses this option unless the environment variable HALOAP_EXPERIMENTAL=1 is set."""
    display_name = "Enable Halo 2"
    visibility = _H2_VISIBILITY
    default = False


class H2FinalMission(Choice):
    """EXPERIMENTAL (see h2_enabled). What the goal mission for Halo 2 is. The Heretic is a cutscene and is never a mission."""
    display_name = "Final Halo 2 Mission"
    visibility = _H2_VISIBILITY
    default = 15
    option_the_armory = 2
    option_cairo_station = 3
    option_outskirts = 4
    option_metropolis = 5
    option_the_arbiter = 6
    option_the_oracle = 7
    option_delta_halo = 8
    option_regret = 9
    option_sacred_icon = 10
    option_quarantine_zone = 11
    option_gravemind = 12
    option_uprising = 13
    option_high_charity = 14
    option_the_great_journey = 15
    option_random_choice = 16


@dataclass
class MCCOptions(PerGameCommonOptions):
    skullsanity: SkullSanity
    kill_milestone_interval: KillMilestoneInterval
    kill_milestone_max_percent: KillMilestoneMaxPercent
    kill_milestone_progression_percent: KillMilestoneProgressionPercent
    kill_difficulty: KillDifficulty
    kill_species_checks: KillSpeciesChecks
    kill_infection_interval: KillInfectionInterval
    skulls_required: SkullsRequired
    # powerups: Powerups
    ce_enabled: CeEnabled
    ce_final_mission: CeFinalMission
    h2_enabled: H2Enabled
    h2_final_mission: H2FinalMission
