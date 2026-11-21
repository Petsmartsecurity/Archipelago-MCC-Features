from dataclasses import dataclass
from Options import PerGameCommonOptions, Toggle, Choice, Range


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
    a script removes count too; Flood infection forms do not.

    0 turns kill milestone checks off. Smaller numbers mean many more checks (1 is a check for every single kill
    up to the cap, which is hundreds per mission). The highest milestone in a mission is set by
    kill_milestone_max_percent and kill_difficulty.
    """
    display_name = "Kill Milestone Interval"
    range_start = 0
    range_end = 100
    default = 0


class KillMilestoneMaxPercent(Range):
    """
    The highest kill milestone in a mission is at most this percent of that mission's enemy count, so every
    milestone can be reached in a single run of the mission. For example 50 with Normal enemy counts on a mission
    with 143 enemies means milestones up to 71 kills. Only used when kill_milestone_interval is above 0.
    """
    display_name = "Kill Milestone Max Percent"
    range_start = 10
    range_end = 90
    default = 50


class KillDifficulty(Choice):
    """
    Which enemy count kill_milestone_max_percent is taken from.

    normal:    The smaller of the Normal and Legendary enemy counts (a few missions have fewer enemies on
               Legendary). Reachable whatever difficulty you play. Choose this on Heroic too; Heroic counts have
               not been measured yet.
    legendary: The Legendary difficulty enemy count. Can allow more milestones, but they are only reachable if
               you play on Legendary.
    """
    display_name = "Kill Difficulty"
    option_normal = 0
    option_legendary = 1
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


@dataclass
class MCCOptions(PerGameCommonOptions):
    skullsanity: SkullSanity
    kill_milestone_interval: KillMilestoneInterval
    kill_milestone_max_percent: KillMilestoneMaxPercent
    kill_difficulty: KillDifficulty
    kill_species_checks: KillSpeciesChecks
    skulls_required: SkullsRequired
    # powerups: Powerups
    ce_enabled: CeEnabled
    ce_final_mission: CeFinalMission
