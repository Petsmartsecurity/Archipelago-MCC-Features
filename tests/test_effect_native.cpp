// Plain C++ tests for the pure logic of the CE native effects (hooks/effect_native_logic.h, ADR-005 phase B): op names, the request
// slot, the weapon chains (QA M4), the small numbers, and how native rows behave in the effect queue.
#include "hooks/effect_native_logic.h"
#include "hooks/effect_queue.h"

#include <cstdio>
#include <cstring>
#include <set>
#include <string>

using namespace haloap::fx;

static int g_checks = 0;
static int g_failures = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static Gate Playing() { Gate g; g.inMission = true; g.engineReady = true; return g; }

int main() {
    // op names: every row "native:<x>" maps to exactly one op, unknown names map to None
    CHECK(IsNativeCommand("native:sniper_drop"));
    CHECK(!IsNativeCommand("(set cheat_x true)"));
    CHECK(!IsNativeCommand(""));
    CHECK(OpFromCommand("native:shields_down") == Op::ShieldsDown);
    CHECK(OpFromCommand("native:nope") == Op::None);
    CHECK(OpFromCommand("(game_speed 1)") == Op::None);
    CHECK(OpFromCommand("native:") == Op::None);
    {
        std::set<std::string> names;
        std::set<int> ops;
        for (const auto& n : kOpNames) { names.insert(n.name); ops.insert(static_cast<int>(n.op)); CHECK(OpFromCommand(std::string("native:") + n.name) == n.op); }
        CHECK(names.size() == sizeof(kOpNames) / sizeof(kOpNames[0]));
        CHECK(ops.size() == names.size());
        CHECK(static_cast<int>(Op::Count) - 1 == static_cast<int>(names.size()));  // every op has a name
        CHECK(std::strcmp(OpLabel(Op::AmmoRefill), "ammo_refill") == 0);
    }
    // every native row of the table names a real op, is a one-shot, and traps are exactly the trap op
    {
        int natives = 0;
        for (size_t i = 0; i < kEffectCount; i++) {
            const Op op = OpFromCommand(kEffects[i].apply);
            if (!IsNativeCommand(kEffects[i].apply)) continue;
            natives++;
            CHECK(op != Op::None);
            CHECK(kEffects[i].durationMs == 0 && !kEffects[i].undo[0]);
            CHECK(kEffects[i].trap == IsTrapOp(op));
        }
        CHECK(natives == 9);
    }
    CHECK(IsDrop(Op::SniperDrop) && IsDrop(Op::MagnumDrop) && !IsDrop(Op::Overshield));
    CHECK(NeedsOnFoot(Op::SniperDrop) && NeedsOnFoot(Op::AmmoRefill) && NeedsOnFoot(Op::GrenadePack));
    CHECK(!NeedsOnFoot(Op::Overshield) && !NeedsOnFoot(Op::ActiveCamo) && !NeedsOnFoot(Op::ShieldsDown));

    // request slot: one execution per request; cancel only before the start; take only when done
    {
        OpSlot s;
        Op op = Op::None;
        Result r = Result::Failed;
        const char* why = nullptr;
        CHECK(!s.TryStart(&op));                        // nothing requested: a console-typed cheat does nothing
        CHECK(s.Request(Op::Overshield));
        CHECK(!s.Request(Op::ActiveCamo));              // busy
        CHECK(s.Peek() == OpSlot::Requested);
        CHECK(s.TryStart(&op) && op == Op::Overshield);
        CHECK(!s.TryStart(&op));                        // a second evaluation of the same request does not run it again
        CHECK(!s.Cancel());                             // already running: not cancellable
        CHECK(!s.Take(&r, &why));
        s.Finish(Result::Deferred, "dead");
        CHECK(s.Take(&r, &why) && r == Result::Deferred && std::strcmp(why, "dead") == 0);
        CHECK(!s.Take(&r, &why));
        CHECK(s.Peek() == OpSlot::Idle);
        CHECK(s.Request(Op::GrenadePack));
        CHECK(s.Cancel());                              // never started: gone
        CHECK(!s.TryStart(&op));
        CHECK(s.Request(Op::GrenadePack));              // slot is reusable
        s.Reset();
        CHECK(s.Peek() == OpSlot::Idle);
    }
    CHECK(!OpTimedOut(1000, 500));
    CHECK(OpTimedOut(500 + kOpTimeoutMs, 500));
    CHECK(!OpTimedOut(400, 500));                       // a start "in the future" (two threads' clocks) is not a timeout

    // weapon chains (QA M4): exact names only, the wanted weapon first, never hunter fuel rod / energy sword
    {
        CHECK(IsPlayerWeaponName("weapons\\sniper rifle\\sniper rifle"));
        CHECK(IsPlayerWeaponName("weapons\\fuel rod gun\\fuel rod"));
        CHECK(!IsPlayerWeaponName("weapons\\fuel rod gun\\hunter fuel rod"));
        CHECK(!IsPlayerWeaponName("weapons\\energy sword\\energy sword"));
        CHECK(!IsPlayerWeaponName("vehicles\\warthog\\warthog gun"));
        CHECK(!IsPlayerWeaponName("characters\\sentinel\\sentinel"));
        CHECK(!IsPlayerWeaponName("weapons\\sniper rifle\\sniper rifle "));  // exact: no prefix / suffix matches
        CHECK(!IsPlayerWeaponName(nullptr));
        const Op drops[] = { Op::SniperDrop, Op::ShotgunDrop, Op::RocketDrop, Op::MagnumDrop };
        for (Op op : drops) {
            const Chain c = ChainFor(op);
            CHECK(c.count >= 2 && c.count <= 8);
            for (size_t i = 0; i < c.count; i++) CHECK(IsPlayerWeaponName(c.names[i]));
            // every chain ends in a weapon every campaign map has (assault rifle), so a drop never finds nothing
            CHECK(std::strcmp(c.names[c.count - 1], kWAssaultRifle) == 0);
        }
        CHECK(ChainFor(Op::Overshield).count == 0);
        CHECK(std::strcmp(ChainFor(Op::SniperDrop).names[0], kWSniper) == 0);
        CHECK(std::strcmp(ChainFor(Op::MagnumDrop).names[0], kWMagnum) == 0);
        // map coverage from the research (4.5): sniper present -> sniper; missing -> next in the chain that exists
        auto pickName = [](Op op, const std::set<std::string>& present) -> std::string {
            const Chain c = ChainFor(op);
            int i = PickFromChain(c, [&](size_t k) { return present.count(c.names[k]) != 0; });
            return i < 0 ? "" : c.names[i];
        };
        const std::set<std::string> a10 = { kWAssaultRifle, kWMagnum, kWPlasmaPistol, kWPlasmaRifle };
        const std::set<std::string> b30 = { kWAssaultRifle, kWMagnum, kWPlasmaPistol, kWPlasmaRifle, kWNeedler, kWRocket,
                                            "weapons\\energy sword\\energy sword", "weapons\\fuel rod gun\\hunter fuel rod" };
        const std::set<std::string> a30 = { kWAssaultRifle, kWMagnum, kWPlasmaPistol, kWPlasmaRifle, kWNeedler, kWSniper };
        CHECK(pickName(Op::SniperDrop, a30) == kWSniper);
        CHECK(pickName(Op::SniperDrop, b30) == kWRocket);        // b30 has no sniper: rocket, not the sword or the hunter's fuel rod
        CHECK(pickName(Op::SniperDrop, a10) == kWPlasmaRifle);   // a10: the weak map
        CHECK(pickName(Op::ShotgunDrop, a30) == kWNeedler);
        CHECK(pickName(Op::RocketDrop, a10) == kWPlasmaRifle);
        CHECK(pickName(Op::RocketDrop, b30) == kWRocket);
        CHECK(pickName(Op::MagnumDrop, a10) == kWMagnum);
        CHECK(pickName(Op::SniperDrop, { "weapons\\energy sword\\energy sword", "weapons\\fuel rod gun\\hunter fuel rod" }).empty());
        CHECK(pickName(Op::SniperDrop, {}).empty());
    }

    // numbers
    CHECK(GrenadesAfterPack(0) == 2 && GrenadesAfterPack(2) == 4 && GrenadesAfterPack(3) == 4 && GrenadesAfterPack(4) == 4);
    CHECK(GrenadesAfterPack(-5) == 2 && GrenadesAfterPack(200) == 4);
    static_assert(kShieldsDownStunTicks == 150, "5 s at 30 ticks/s");
    static_assert(kShieldsDownStunTicks <= 32767, "i16 field");
    static_assert(kCamoTicks == 900, "30 s at 30 ticks/s");
    CHECK(std::strcmp(CueFor(Op::ShieldsDown), kCueTrap) == 0 && std::strcmp(CueFor(Op::Overshield), kCueGood) == 0);
    static_assert(kClassWeapon == 0x77656170 && kClassSound == 0x736e6421, "fourcc as the engine compares them");

    // native rows in the queue: a deferral (Failed) keeps the apply at the front, does not consume the 10 s gap, retries after 2 s
    {
        EffectQueue q;
        q.Tick(0, Playing());
        q.Add(81003, 1);
        q.Add(81001, 2);
        auto a = q.Tick(kSettleMs, Playing());
        CHECK(a.size() == 1 && a[0].kind == Action::Apply && a[0].id == 81003 && IsNativeCommand(a[0].command));
        CHECK(OpFromCommand(a[0].command) == Op::SniperDrop);
        q.Failed(a[0], kSettleMs + 100);                          // gateway answered Deferred (no unit yet)
        CHECK(q.Tick(kSettleMs + 200, Playing()).empty());        // retry delay
        auto b = q.Tick(kSettleMs + 100 + kRetryMs, Playing());
        CHECK(b.size() == 1 && b[0].index == 1);                  // first in line again, gap untouched
        CHECK(q.Add(81003, 1) == Enqueue::AlreadyApplied);
    }
    // a native op that was applied is not repeated by a re-send of the same index
    {
        EffectQueue q;
        q.Tick(0, Playing());
        q.Add(81101, 7);
        auto a = q.Tick(kTrapSettleMs, Playing());
        CHECK(a.size() == 1 && OpFromCommand(a[0].command) == Op::ShieldsDown);
        CHECK(q.Add(81101, 7) == Enqueue::AlreadyApplied);
    }

    std::printf("test_effect_native: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
