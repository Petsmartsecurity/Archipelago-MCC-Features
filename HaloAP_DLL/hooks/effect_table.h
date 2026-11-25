#pragma once
// The one table of Halo CE effect items (ADR-005, docs/research/item-effects.md 2.4 / 4.2). Pure data, no includes beyond
// <cstddef>, so the native tests, tools/h1/check_effect_commands.py (gate `effect-commands`: every HaloScript name below must
// exist in halo1.dll's script table) and tests/test_effect_ids_agree.py (ids/names/trap flags == apworld items.py) all read
// this same file. Keep every row on ONE line: the Python readers parse them with a regex.
//
// Row = { AP item id, name, apply command, undo command ("" = none), duration in ms (0 = one-shot), undo group, trap }.
// Commands are HaloScript expressions without the "HS: " prefix (PostHaloScript adds it). Timed effects of the same group
// replace each other (a later gravity effect cancels the earlier one's timer, so the first undo cannot cut the second short).
// Not in the table on purpose: 81002 Recharged Shields (retired: object_set_shield writes frac * max shield into a 0..1 fraction,
// so `1` is a huge overshield and no other value is safe across maps; QA item-effects-a M2).
// Also not: 81103 (explosion fallback via damage_object): the damage is not capped, so it can kill from low
// health (research 3.1.6); phase A has no health read to guard it. Phase B items (81003..81010, 81102, 81106) need the frame hook.
// 81101 does use damage_object, but only with the one 10-point, no-area tag "weapons\assault rifle\bullet" (gate effect-commands
// pins the tag, its values and its presence in all 10 maps): the hit restarts the shield recharge delay, then the shield is set to 0.
#include <cstddef>

namespace haloap {
namespace fx {

    struct EffectDef {
        int id;
        const char* name;
        const char* apply;
        const char* undo;
        int durationMs;
        const char* group;
        bool trap;
    };

    // player 0's unit; "(unit (list_get (players) 0))" is the research doc's spelling (2.1: `unit` is the object -> unit cast)
    constexpr EffectDef kEffects[] = {
        { 81001, "Medkit", "(unit_set_current_vitality (unit (list_get (players) 0)) 9999 9999)", "", 0, "", false },
        { 81011, "Invulnerable", "(set cheat_deathless_player true)", "(set cheat_deathless_player false)", 20000, "deathless", false },
        { 81012, "Infinite Ammo", "(set cheat_infinite_ammo true)", "(set cheat_infinite_ammo false)", 30000, "ammo", false },
        { 81013, "Moon Gravity", "(physics_set_gravity 0.4)", "(physics_constants_reset)", 30000, "gravity", false },
        { 81101, "Shields Down", "(begin (damage_object \"weapons\\assault rifle\\bullet\" (list_get (players) 0)) (object_set_shield (list_get (players) 0) 0))", "", 0, "", true },
        { 81104, "Heavy Gravity", "(physics_set_gravity 2.0)", "(physics_constants_reset)", 20000, "gravity", true },
        { 81105, "Slow Motion", "(game_speed 0.5)", "(game_speed 1)", 15000, "speed", true },
    };
    constexpr size_t kEffectCount = sizeof(kEffects) / sizeof(kEffects[0]);

    inline const EffectDef* FindEffect(int id) {
        for (const auto& e : kEffects) if (e.id == id) return &e;
        return nullptr;
    }

}  // namespace fx
}  // namespace haloap
