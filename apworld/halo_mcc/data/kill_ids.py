"""Kill-milestone identity (ADR-001 Amendment A). Hand-written; stdlib only, so the repo tests can import it directly.

A milestone's identity is (mission, kill count k), so any interval maps onto the same IDs:
    id = 1_000_000_000 + game*10_000_000 + (100 + mission_no)*10_000 + k        k = 1..9999
The launcher (HaloAP_launcher/kill_tracker.h, MilestoneId) uses the same formula; tests/test_kill_ids_agree.py
fails if either side drifts. mission_no is 1..10 in campaign order (a10, a30, a50, b30, b40, c10, c20, c40, d20, d40).
"""

KILL_BASE = 1_000_000_000
GAME_CE = 1
MAX_K = 9999
DATAPACKAGE_PERCENT = 90  # the largest kill_milestone_max_percent any seed can ask for


def milestone_id(mission_no: int, k: int) -> int:
    if not (1 <= mission_no <= 10 and 1 <= k <= MAX_K):
        raise ValueError(f"milestone out of range: mission {mission_no}, k {k}")
    return KILL_BASE + GAME_CE * 10_000_000 + (100 + mission_no) * 10_000 + k


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
