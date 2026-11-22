from rule_builder.rules import *

def set_goal(world):
    # the goal is every enabled game's final mission
    finals = [f for f in (world.final_mission, world.h2_final_mission) if f]
    rules = [CanReachEntrance(f"Menu -> {f}") for f in finals]
    world.set_completion_rule(rules[0] if len(rules) == 1 else And(*rules))
