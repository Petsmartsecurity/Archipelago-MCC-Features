#pragma once
// Pure logic of the CE native effects (ADR-005 phase B). No game memory, no Windows: mcc-ap/tests/test_effect_native.cpp drives it.
//
// What lives here: the op names (the table rows say "native:<name>", tools/h1/check_effect_commands.py ties them to kOpNames), the
// request slot that carries one op from the worker thread to the gateway detour (a four-state machine, lock-free, at most one
// execution per request), the exact-name weapon chains (QA M4: only weapons the player can hold; never the hunter fuel rod or the
// energy sword), and the small numbers (grenade cap, stun ticks, cue names).
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace haloap {
namespace fx {

    enum class Op : int {
        None = 0,
        SniperDrop, ShotgunDrop, MagnumDrop, RocketDrop,
        Overshield, ActiveCamo, GrenadePack, AmmoRefill,
        ShieldsDown,
        Count
    };

    struct OpName { const char* name; Op op; };
    // Row text in effect_table.h is "native:<name>"; the gate requires every such name to appear here as a quoted literal.
    constexpr OpName kOpNames[] = {
        { "sniper_drop", Op::SniperDrop },
        { "shotgun_drop", Op::ShotgunDrop },
        { "magnum_drop", Op::MagnumDrop },
        { "rocket_drop", Op::RocketDrop },
        { "overshield", Op::Overshield },
        { "active_camo", Op::ActiveCamo },
        { "grenade_pack", Op::GrenadePack },
        { "ammo_refill", Op::AmmoRefill },
        { "shields_down", Op::ShieldsDown },
    };

    constexpr std::string_view kNativePrefix = "native:";
    inline bool IsNativeCommand(std::string_view cmd) { return cmd.substr(0, kNativePrefix.size()) == kNativePrefix; }
    inline Op OpFromCommand(std::string_view cmd) {
        if (!IsNativeCommand(cmd)) return Op::None;
        cmd.remove_prefix(kNativePrefix.size());
        for (const auto& n : kOpNames) if (cmd == n.name) return n.op;
        return Op::None;
    }
    inline const char* OpLabel(Op op) {
        for (const auto& n : kOpNames) if (n.op == op) return n.name;
        return "?";
    }
    inline bool IsDrop(Op op) { return op == Op::SniperDrop || op == Op::ShotgunDrop || op == Op::MagnumDrop || op == Op::RocketDrop; }
    // ops that put something into the world or into a hand: not while the player sits in a vehicle (m5)
    inline bool NeedsOnFoot(Op op) { return IsDrop(op) || op == Op::AmmoRefill || op == Op::GrenadePack; }
    inline bool IsTrapOp(Op op) { return op == Op::ShieldsDown; }

    // ---- results --------------------------------------------------------------------------------------------------------
    enum class Result : int {
        Applied = 0,       // done: ack
        Deferred,          // the world is not ready (no unit, dead, cutscene, seated ...): the queue retries
        Unsupported,       // this build lacks a native the op needs (pattern missing): item disabled, ack as skipped
        Failed,            // an exception or an unexpected engine answer inside the op: retry later
    };

    // ---- request slot ---------------------------------------------------------------------------------------------------
    // One writer (the worker thread: Request / Poll / Cancel), one executor (the gateway detour: TryStart / Finish). An op runs
    // at most once per request: only the Requested -> Running transition lets the detour execute, and Cancel only succeeds
    // from Requested.
    class OpSlot {
    public:
        enum State : uint32_t { Idle = 0, Requested = 1, Running = 2, Done = 3 };
        bool Request(Op op) {
            if (m_state.load(std::memory_order_acquire) != Idle) return false;
            m_op.store(static_cast<int>(op), std::memory_order_relaxed);
            m_result.store(static_cast<int>(Result::Failed), std::memory_order_relaxed);
            m_why.store("", std::memory_order_relaxed);
            m_state.store(Requested, std::memory_order_release);
            return true;
        }
        // detour side
        bool TryStart(Op* op) {
            uint32_t e = Requested;
            if (!m_state.compare_exchange_strong(e, Running, std::memory_order_acq_rel)) return false;
            *op = static_cast<Op>(m_op.load(std::memory_order_relaxed));
            return true;
        }
        void Finish(Result r, const char* why) {
            m_result.store(static_cast<int>(r), std::memory_order_relaxed);
            m_why.store(why ? why : "", std::memory_order_relaxed);
            m_state.store(Done, std::memory_order_release);
        }
        // worker side
        State Peek() const { return static_cast<State>(m_state.load(std::memory_order_acquire)); }
        // Done -> Idle, returns the result; false if not done yet
        bool Take(Result* r, const char** why) {
            if (m_state.load(std::memory_order_acquire) != Done) return false;
            *r = static_cast<Result>(m_result.load(std::memory_order_relaxed));
            *why = m_why.load(std::memory_order_relaxed);
            m_state.store(Idle, std::memory_order_release);
            return true;
        }
        // true if the request never started (the detour will not run it); false if it already runs or ran
        bool Cancel() {
            uint32_t e = Requested;
            return m_state.compare_exchange_strong(e, Idle, std::memory_order_acq_rel);
        }
        void Reset() { m_state.store(Idle, std::memory_order_release); }
    private:
        std::atomic<uint32_t> m_state{ Idle };
        std::atomic<int> m_op{ 0 };
        std::atomic<int> m_result{ 0 };
        std::atomic<const char*> m_why{ "" };
    };

    // ---- in-flight bookkeeping (worker side) ----------------------------------------------------------------------------
    constexpr uint64_t kOpTimeoutMs = 3000;  // the frame pump runs every frame; a request that never ran by then was lost
    inline bool OpTimedOut(uint64_t nowMs, uint64_t startMs) { return nowMs >= startMs && nowMs - startMs >= kOpTimeoutMs; }

    // ---- weapons --------------------------------------------------------------------------------------------------------
    // Exact tag names (class weap) of weapons the player can hold in the campaign (QA M4). Anything else, in particular
    // `weapons\fuel rod gun\hunter fuel rod` (magazine 0) and `weapons\energy sword\energy sword` (no magazine), is never chosen.
    constexpr const char* kWAssaultRifle = "weapons\\assault rifle\\assault rifle";
    constexpr const char* kWMagnum = "weapons\\pistol\\pistol";
    constexpr const char* kWPlasmaPistol = "weapons\\plasma pistol\\plasma pistol";
    constexpr const char* kWPlasmaRifle = "weapons\\plasma rifle\\plasma rifle";
    constexpr const char* kWNeedler = "weapons\\needler\\needler";
    constexpr const char* kWSniper = "weapons\\sniper rifle\\sniper rifle";
    constexpr const char* kWShotgun = "weapons\\shotgun\\shotgun";
    constexpr const char* kWRocket = "weapons\\rocket launcher\\rocket launcher";
    constexpr const char* kWFlamethrower = "weapons\\flamethrower\\flamethrower";
    constexpr const char* kWFuelRod = "weapons\\fuel rod gun\\fuel rod";
    constexpr const char* kPlayerWeapons[] = { kWAssaultRifle, kWMagnum, kWPlasmaPistol, kWPlasmaRifle, kWNeedler, kWSniper, kWShotgun,
                                               kWRocket, kWFlamethrower, kWFuelRod };

    // Fallback chains (research 4.5, QUESTIONS #17): the first name present in the loaded map wins. The wanted weapon first, then
    // the closest stand-ins; every chain ends in something every campaign map has (assault rifle, plasma rifle, pistol).
    constexpr const char* kChainSniper[] = { kWSniper, kWRocket, kWShotgun, kWFuelRod, kWNeedler, kWPlasmaRifle, kWAssaultRifle };
    constexpr const char* kChainShotgun[] = { kWShotgun, kWNeedler, kWPlasmaRifle, kWAssaultRifle };
    constexpr const char* kChainRocket[] = { kWRocket, kWFuelRod, kWPlasmaRifle, kWAssaultRifle };
    constexpr const char* kChainMagnum[] = { kWMagnum, kWAssaultRifle };

    struct Chain { const char* const* names; size_t count; };
    inline Chain ChainFor(Op op) {
        switch (op) {
        case Op::SniperDrop: return { kChainSniper, sizeof(kChainSniper) / sizeof(kChainSniper[0]) };
        case Op::ShotgunDrop: return { kChainShotgun, sizeof(kChainShotgun) / sizeof(kChainShotgun[0]) };
        case Op::RocketDrop: return { kChainRocket, sizeof(kChainRocket) / sizeof(kChainRocket[0]) };
        case Op::MagnumDrop: return { kChainMagnum, sizeof(kChainMagnum) / sizeof(kChainMagnum[0]) };
        default: return { nullptr, 0 };
        }
    }
    inline bool IsPlayerWeaponName(const char* name) {
        if (!name) return false;
        for (const char* w : kPlayerWeapons) if (std::strcmp(name, w) == 0) return true;
        return false;
    }
    // Index into the chain of the first name for which present(i) says the map has that tag; -1 if none.
    template <class Present>
    inline int PickFromChain(const Chain& c, Present present) {
        for (size_t i = 0; i < c.count; i++) if (IsPlayerWeaponName(c.names[i]) && present(i)) return static_cast<int>(i);
        return -1;
    }

    // ---- numbers --------------------------------------------------------------------------------------------------------
    constexpr int kGrenadeCap = 4;        // globals "maximum count" of both grenade types in all 10 maps (QA m2)
    constexpr int kGrenadePackAdds = 2;   // per type
    inline int GrenadesAfterPack(int current) {
        int n = current < 0 ? 0 : current;
        n += kGrenadePackAdds;
        return n > kGrenadeCap ? kGrenadeCap : n;
    }
    constexpr int kShieldsDownStunTicks = 150;  // 5 s at 30 ticks/s (research 3.3): the shield stays empty, no recharge
    constexpr float kOvershieldRefill = 3.0f;   // second overshield while one runs: back to full (decay handles the rest)
    constexpr int kCamoTicks = 900;             // 30 s at 30 ticks/s (the eqip's own powerup time)

    // sound cues: tag names present in all 10 campaign maps (research 5.1); a trap gets the depleted cue
    constexpr const char* kCueGood = "sound\\sfx\\ui\\pickup_health";
    constexpr const char* kCueTrap = "sound\\sfx\\ui\\shield_depleted";
    inline const char* CueFor(Op op) { return IsTrapOp(op) ? kCueTrap : kCueGood; }

    // class codes as the engine compares them (0xb805b0 compares 'snd!' with 0x736e6421)
    constexpr uint32_t kClassWeapon = 0x77656170;  // 'weap'
    constexpr uint32_t kClassSound = 0x736e6421;   // 'snd!'

}  // namespace fx
}  // namespace haloap
