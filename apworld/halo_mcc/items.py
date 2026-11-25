from BaseClasses import Item, ItemClassification
from .data.constants import *
from .data.levels import LEVEL_DATA, LevelData
from .data.skulls import GAME_SKULLS, NON_SCORING, NON_SCORING_SKULLS
from .data.halo2_skull_data import H2_SKULL_ITEMS, H2_SKULL_NAMES
from .mcc_options import SkullSanity

# Ordered list of CE skull disabler items (non-PERM_DISABLED CE skulls, alphabetical)
CE_SKULL_DISABLERS: list[str] = [*GAME_SKULLS["ce"], *NON_SCORING_SKULLS["ce"]]


# Halo 2 skull items (QUESTIONS #16): one per engine skull, id 92000 + MCC bit, "<Skull> Skull (Halo 2)". Same tiers as CE:
# scoring skulls (not in NON_SCORING) come with "all"/"inverted", 1.00x skulls with every tier above off.
H2_SKULL_SCORING: list[str] = [f"{n} Skull (Halo 2)" for n in H2_SKULL_NAMES if n not in NON_SCORING]
H2_SKULL_NONSCORING: list[str] = [f"{n} Skull (Halo 2)" for n in H2_SKULL_NAMES if n in NON_SCORING]

# Classification: good effects are `filler`, not `useful`: kill locations above kill_milestone_progression_percent are EXCLUDED and
# only accept filler/trap, so a pool of `useful` effects made generation fail ("Not enough filler items for excluded locations").
# Effect items (ADR-005, docs/research/item-effects.md 4.1/4.2): ids 80000 + game*1000 + k (CE = 81000..81999), good k 1..99,
# traps k 101..199. Append-only once released: never renumber or rename. Gaps are intended (81102 Plasma Pistol Swap, 81103 Live Grenade,
# 81106 Disarmed and 81107 Skull Trap are phase B items not built yet). The DLL runs the matching HaloScript or native op (hooks/effect_table.h; tests/test_effect_ids_agree.py ties
# the two). Weight = relative chance among the good items (or among the traps) when filler is replaced.
# Retired before release use (ADR-005 After QA M2): keeps its id and name in the data package (append-only) but is never generated
# and the DLL has no command for it. `object_set_shield` writes frac * max shield into a 0..1 fraction, so no value is safely "full".
# Heavy Gravity and Slow Motion: dropped by the user (QUESTIONS #15, 2026-11-24). Retired the same way; the DLL keeps their rows so
# a seed generated before this change still applies them.
CE_EFFECT_RETIRED: dict[str, tuple[int, ItemClassification]] = {
    "Effect: Recharged Shields": (81002, ItemClassification.filler),
    "Trap: Heavy Gravity (20 s)": (81104, ItemClassification.trap),
    "Trap: Slow Motion (15 s)": (81105, ItemClassification.trap),
}
CE_EFFECT_ITEMS: list[tuple[str, int, ItemClassification, int]] = [
    ("Effect: Medkit", 81001, ItemClassification.filler, 3),
    ("Effect: Invulnerable (20 s)", 81011, ItemClassification.filler, 2),
    ("Effect: Infinite Ammo (30 s)", 81012, ItemClassification.filler, 2),
    ("Effect: Moon Gravity (30 s)", 81013, ItemClassification.filler, 2),
    # phase B (native ops through the DLL's gateway, ADR-005): weapon drops land on the ground next to the player with full ammo and
    # fall back to a similar weapon on maps that lack the wanted one (the DLL logs what dropped); the power weapons are rarer
    ("Effect: Sniper Rifle Drop", 81003, ItemClassification.filler, 1),
    ("Effect: Shotgun Drop", 81004, ItemClassification.filler, 1),
    ("Effect: Magnum Drop", 81005, ItemClassification.filler, 2),
    ("Effect: Rocket Launcher Drop", 81006, ItemClassification.filler, 1),
    ("Effect: Overshield", 81007, ItemClassification.filler, 2),
    ("Effect: Active Camo", 81008, ItemClassification.filler, 2),
    ("Effect: Grenade Pack", 81009, ItemClassification.filler, 3),
    ("Effect: Ammo Refill", 81010, ItemClassification.filler, 3),
    ("Trap: Shields Down", 81101, ItemClassification.trap, 3),
]
EFFECT_BY_NAME = {**CE_EFFECT_RETIRED, **{name: (item_id, cls) for name, item_id, cls, _ in CE_EFFECT_ITEMS}}


def effects_enabled(world) -> bool:
    # CE-only: the DLL effects exist for Halo CE; without CE nothing could apply them
    return bool(world.options.effect_items) and bool(world.options.ce_enabled)


# create our own item object that has the game set correctly, everything else is the same as the base item object
class MCCItem(Item):
    game = "Halo The Master Chief Collection"


# this is what maps items to their ID
def get_item_name_to_id():
    item_table = {
        "filler": 1,
        **{f"{level} Access": data.offset for level, data in LEVEL_DATA.items()},
        **{f"{skull} Skull": SKULL_OFFSET + i + 1 for i, skull in enumerate(CE_SKULL_DISABLERS)},
        **{name: item_id for name, (item_id, _) in EFFECT_BY_NAME.items()},
        **{name: item_id for name, (item_id, _b, _n) in H2_SKULL_ITEMS.items()},
    }
    return item_table


# gives us the name of a filler item
def get_filler_item_name(world):
    if not effects_enabled(world):
        return "filler"  # no random draw: a seed without effect_items keeps its exact RNG stream (ce_golden)
    traps = [i for i in CE_EFFECT_ITEMS if i[2] == ItemClassification.trap]
    good = [i for i in CE_EFFECT_ITEMS if i[2] != ItemClassification.trap]
    pool = traps if world.random.random() * 100 < world.options.trap_percentage.value else good
    return world.random.choices([i[0] for i in pool], weights=[i[3] for i in pool])[0]


# fill unfilled locations with filler from this world
def create_filler(world, filled_locations):
    fillerpool: list[Item] = []
    unfilled_locations = world.multiworld.get_unfilled_locations(world.player)
    needed_items = len(unfilled_locations) - filled_locations
    for i in range(needed_items):
        fillerpool.append(create_item_with_data(world, get_filler_item_name(world)))

    return fillerpool


# create all items for the world
def create_items(world):
    itempool: list[Item] = []
    finals = (world.final_mission, world.h2_final_mission)
    for level, data in LEVEL_DATA.items():
        if data.game not in world.enabled_games():
            continue
        if level not in finals:
            if level == world.starting_mission:
                world.multiworld.push_precollected(create_item_with_data(world, f"{level} Access"))
            else:
                itempool.append(create_item_with_data(world, f"{level} Access"))

    # skull items are CE-only
    # if option has all skulls on
    if world.options.ce_enabled and world.options.skullsanity >= 2:
        for skull in GAME_SKULLS["ce"]:
            itempool.append(create_item_with_data(world, f"{skull} Skull"))

    # if option has only non-scoring skulls on
    if world.options.ce_enabled and world.options.skullsanity >= 1:
        for skull in NON_SCORING_SKULLS["ce"]:
            itempool.append(create_item_with_data(world, f"{skull} Skull"))

    # Halo 2 skull items (separate from CE's, QUESTIONS #16)
    if world.options.h2_enabled and world.options.h2_skullsanity >= 2:
        for name in H2_SKULL_SCORING:
            itempool.append(create_item_with_data(world, name))
    if world.options.h2_enabled and world.options.h2_skullsanity >= 1:
        for name in H2_SKULL_NONSCORING:
            itempool.append(create_item_with_data(world, name))

    itempool.extend(create_filler(world, len(itempool)))

    world.multiworld.itempool += itempool


# creates a single item with its classification and ID
def create_item_with_data(world, name):
    if name in EFFECT_BY_NAME:
        item_id, classification = EFFECT_BY_NAME[name]
        return MCCItem(name, classification, item_id, world.player)
    if name in H2_SKULL_ITEMS:
        item_id = H2_SKULL_ITEMS[name][0]
        scoring = name in H2_SKULL_SCORING
        return MCCItem(name, ItemClassification.progression if scoring else ItemClassification.useful, item_id, world.player)
    if "Access" in name:
        classification = ItemClassification.progression
        real_name = name.replace(" Access", "")
        level_data = LEVEL_DATA[real_name]
        item_id = level_data.offset
    elif "Skull" in name:
        skull = name[:-len(" Skull"):]
        # Non-scoring skulls and Catch are marked useful rather than progression.
        # CE has exactly 13 scoring skull disablers matching the 13 skull locations;
        if skull in NON_SCORING:
            classification = ItemClassification.useful
        else:
            classification = ItemClassification.progression
        return MCCItem(name, classification, SKULL_OFFSET + CE_SKULL_DISABLERS.index(skull) + 1, world.player)

    else:
        classification = ItemClassification.filler
        item_id = 1

    return MCCItem(name, classification, item_id, world.player)
