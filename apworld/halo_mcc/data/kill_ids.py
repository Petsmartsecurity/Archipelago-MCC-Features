"""Kill-milestone and infection-form identity (ADR-001 Amendments A and B). Hand-written; stdlib only, so the repo tests can import it directly.

A milestone's identity is (mission, kill count k), so any interval maps onto the same IDs:
    id = 1_000_000_000 + game*10_000_000 + (100 + mission_no)*10_000 + k        k = 1..9999
Infection-form checks (Flood swarm deaths, Amendment B) have their own block, so they can never meet a milestone or species ID:
    id = 1_000_000_000 + game*10_000_000 + (200 + mission_no)*10_000 + k
The launcher (HaloAP_launcher/kill_tracker.h, MilestoneId / InfectionId) uses the same formulas; tests/test_kill_ids_agree.py
fails if either side drifts. mission_no is 1..10 in campaign order (a10, a30, a50, b30, b40, c10, c20, c40, d20, d40).
"""

KILL_BASE = 1_000_000_000
GAME_CE = 1
MAX_K = 9999
DATAPACKAGE_PERCENT = 90  # the largest kill_milestone_max_percent EVER offered (Amendment A); the data package may only grow
MAX_PERCENT = 80          # the largest kill_milestone_max_percent / infection cap offered now (PLAN 6.R)


def milestone_id(mission_no: int, k: int) -> int:
    if not (1 <= mission_no <= 10 and 1 <= k <= MAX_K):
        raise ValueError(f"milestone out of range: mission {mission_no}, k {k}")
    return KILL_BASE + GAME_CE * 10_000_000 + (100 + mission_no) * 10_000 + k


def infection_id(mission_no: int, k: int) -> int:
    if not (1 <= mission_no <= 10 and 1 <= k <= MAX_K):
        raise ValueError(f"infection check out of range: mission {mission_no}, k {k}")
    return KILL_BASE + GAME_CE * 10_000_000 + (200 + mission_no) * 10_000 + k


def infection_name(mission: str, k: int) -> str:
    return f"{mission} - {k} Infection Form" if k == 1 else f"{mission} - {k} Infection Forms"


def milestone_name(mission: str, k: int) -> str:
    return f"{mission} - {k} Kill" if k == 1 else f"{mission} - {k} Kills"


def milestone_cap(count: int, percent: int) -> int:
    """Highest milestone a seed may create: floor(count * percent / 100)."""
    return count * percent // 100


def datapackage_max_k(normal: int, legendary: int) -> int:
    """Highest k kept in the data package for a mission, whatever the options are. The spec's floor(legendary*90/100)
    is not enough on missions where Legendary has fewer enemies than Normal (a30, a50), so the larger count is used."""
    return milestone_cap(max(normal, legendary), DATAPACKAGE_PERCENT)


def milestone_ks(count: int, interval: int, percent: int):
    """The k values a seed creates: interval, 2*interval, ... <= floor(count * percent / 100). interval 0 = none."""
    if interval <= 0:
        return range(0)
    return range(interval, milestone_cap(count, percent) + 1, interval)


def basis_for(difficulty: str, normal_map: int, legendary_map: int, measured: int) -> int:
    """Enemy count the milestone cap is taken from, for one mission (PLAN 6.R). `measured` = counted kills in one complete
    Normal run (data/h1/kill_basis.json). normal: that. heroic: that too (no Heroic telemetry yet; Heroic spawns between
    Normal and Legendary, so the Normal figure is reachable). legendary: no Legendary telemetry either, so the Normal
    figure, scaled down by Legendary/Normal map count where Legendary has FEWER enemies (a30, a50); never scaled up."""
    if difficulty == "legendary" and legendary_map < normal_map:
        return measured * legendary_map // normal_map
    return measured


def datapackage_milestone_max_k(normal_map: int, legendary_map: int, measured: int) -> int:
    """Highest k kept in the data package: everything Amendment A already published, plus MAX_PERCENT of the measured basis."""
    return max(datapackage_max_k(normal_map, legendary_map), milestone_cap(measured, MAX_PERCENT))


def infection_ks(swarm_basis: int, interval: int):
    """k values a seed creates: interval, 2*interval, ... <= floor(swarm_basis * 80 / 100). Missions with no swarm data: none."""
    if interval <= 0 or swarm_basis <= 0:
        return range(0)
    return range(interval, milestone_cap(swarm_basis, MAX_PERCENT) + 1, interval)


# 80% of min(lowest static per-squad enemy count over all difficulties, measured complete Normal run), per mission:
# the most a milestone may ask for before it can hold NO progression whatever the player does
# (docs/research/h1-per-enemy-checks.md section 5; hand-frozen from that table, a test checks 50% of the basis stays below it).
PROGRESSION_SAFE_CAP = {
    "The Pillar of Autumn": 100, "Halo (CE)": 109, "The Truth and Reconciliation": 244, "The Silent Cartographer": 115,
    "Assault on the Control Room": 307, "343 Guilty Spark": 135, "The Library": 248, "Two Betrayals": 303,
    "Keyes": 251, "The Maw": 148,
}
