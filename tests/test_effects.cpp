// Plain C++ tests for the CE effect queue (hooks/effect_queue.h, ADR-005): hold rules, rate limit, timers, mission change.
#include "hooks/effect_queue.h"

#include <cstdio>
#include <string_view>

using namespace haloap::fx;

static int g_checks = 0;
static int g_failures = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static Gate Playing() { Gate g; g.inMission = true; g.engineReady = true; return g; }

int main() {
    // message parser
    CHECK(ParseEffectMessage("EFFECT: 81001 5")->first == 81001);
    CHECK(ParseEffectMessage("EFFECT: 81001 5")->second == 5);
    CHECK(!ParseEffectMessage("EFFECT: 81001"));
    CHECK(!ParseEffectMessage("EFFECT: 81001 5 7"));
    CHECK(!ParseEffectMessage("EFFECT: 81001 -1"));
    CHECK(!ParseEffectMessage("EFFECT: x 1"));
    CHECK(!ParseEffectMessage("EFFECT:81001 1"));
    CHECK(!ParseEffectMessage("EFFECT: 99999999999 1"));
    CHECK(!ParseEffectMessage("ITEM_RECIVED: 81001 1"));

    // table sanity: unique ids, timed effects have undo + group, one-shots have neither
    for (size_t i = 0; i < kEffectCount; i++) {
        for (size_t j = i + 1; j < kEffectCount; j++) CHECK(kEffects[i].id != kEffects[j].id);
        CHECK(kEffects[i].id >= 81000 && kEffects[i].id < 82000);
        CHECK(kEffects[i].apply[0] == '(' || std::string_view(kEffects[i].apply).substr(0, 7) == "native:");  // phase B rows run native ops
        if (kEffects[i].apply[0] == 'n') CHECK(kEffects[i].durationMs == 0 && !kEffects[i].undo[0]);  // native rows are one-shots
        if (kEffects[i].durationMs > 0) CHECK(kEffects[i].undo[0] == '(' && kEffects[i].group[0]);
        else CHECK(!kEffects[i].undo[0]);
    }

    // add: duplicates, unknown ids, capacity
    {
        EffectQueue q;
        CHECK(q.Add(81001, 1) == Enqueue::Queued);
        CHECK(q.Add(81001, 1) == Enqueue::Duplicate);
        CHECK(q.Add(81011, 1) == Enqueue::Duplicate);  // the index identifies the item
        CHECK(q.Add(12345, 2) == Enqueue::UnknownId);
        CHECK(q.Add(81002, 3) == Enqueue::UnknownId);                  // retired (QA M2): no command, never generated
        for (int i = 10; i < 10 + 63; i++) CHECK(q.Add(81001, i) == Enqueue::Queued);
        CHECK(q.Pending() == 64);
        CHECK(q.Add(81001, 999) == Enqueue::Full);
    }

    // held while not in a mission or engine not ready; applied after the settle delay; never twice
    {
        EffectQueue q;
        q.Add(81001, 1);
        Gate menu;
        CHECK(q.Tick(1000, menu).empty());
        Gate noEngine = Playing(); noEngine.engineReady = false;
        CHECK(q.Tick(2000, noEngine).empty());
        CHECK(q.Tick(3000, Playing()).empty());                       // in a mission since 2000 (engine not ready then): settling
        CHECK(q.Tick(2000 + kSettleMs - 1, Playing()).empty());
        auto a = q.Tick(2000 + kSettleMs, Playing());
        CHECK(a.size() == 1 && a[0].kind == Action::Apply && a[0].id == 81001 && a[0].index == 1);
        CHECK(!a.empty() && a[0].command == FindEffect(81001)->apply);
        CHECK(q.Tick(2000 + kSettleMs + 100, Playing()).empty());
        CHECK(q.Pending() == 0);
        CHECK(q.Add(81001, 1) == Enqueue::AlreadyApplied);            // applied once, never again after a re-send (caller re-acks)
    }

    // dead / cutscene hold the apply (not the timer expiry)
    {
        EffectQueue q;
        q.Add(81001, 1);
        q.Tick(0, Playing());
        Gate dead = Playing(); dead.alive = false;
        CHECK(q.Tick(20000, dead).empty());
        Gate cs = Playing(); cs.cutscene = true;
        CHECK(q.Tick(21000, cs).empty());
        CHECK(q.Pending() == 1);
        CHECK(q.Tick(22000, Playing()).size() == 1);
    }

    // rate limit: one effect per 10 s
    {
        EffectQueue q;
        q.Tick(0, Playing());
        q.Add(81001, 1); q.Add(81011, 2); q.Add(81001, 3);
        uint64_t t = kSettleMs;
        CHECK(q.Tick(t, Playing()).size() == 1);
        CHECK(q.Tick(t + kMinGapMs - 1, Playing()).empty());
        auto a = q.Tick(t + kMinGapMs, Playing());
        CHECK(a.size() == 1 && a[0].index == 2);                      // FIFO
        CHECK(q.Tick(t + 2 * kMinGapMs, Playing())[0].index == 3);
    }

    // traps wait longer after a load; a good item behind them may go first
    {
        EffectQueue q;
        q.Tick(0, Playing());
        q.Add(81101, 1);   // trap
        q.Add(81012, 2);   // good
        auto a = q.Tick(kSettleMs, Playing());
        CHECK(a.size() == 1 && a[0].index == 2);
        static_assert(kSettleMs + kMinGapMs >= kTrapSettleMs, "test timing");
        auto b = q.Tick(kSettleMs + kMinGapMs, Playing());
        CHECK(b.size() == 1 && b[0].index == 1);
        EffectQueue r;                                                 // only a trap queued: held until the trap settle
        r.Tick(0, Playing());
        r.Add(81101, 1);
        CHECK(r.Tick(kTrapSettleMs - 1, Playing()).empty());
        CHECK(r.Tick(kTrapSettleMs, Playing()).size() == 1);
    }

    // timed effect: undo at expiry, undo not blocked by the rate limit
    {
        EffectQueue q;
        q.Tick(0, Playing());
        q.Add(81012, 1);
        auto a = q.Tick(kSettleMs, Playing());
        CHECK(a.size() == 1 && q.ActiveTimers() == 1);
        CHECK(q.Tick(kSettleMs + 29999, Playing()).empty());
        q.Add(81001, 2);
        auto u = q.Tick(kSettleMs + 30000, Playing());
        CHECK(u.size() == 2 && u[0].kind == Action::Undo && u[0].command == "(set cheat_infinite_ammo false)");
        CHECK(u[1].kind == Action::Apply);
        CHECK(q.ActiveTimers() == 0);
        CHECK(q.Tick(kSettleMs + 60000, Playing()).empty());          // the undo is sent once
    }

    // same group replaces: moon gravity then heavy gravity -> one timer, the later one, one undo
    {
        EffectQueue q;
        q.Tick(0, Playing());
        q.Add(81013, 1); q.Add(81104, 2);
        CHECK(q.Tick(kSettleMs, Playing()).size() == 1);               // moon at 8000 (expires 38000 if kept)
        uint64_t t2 = kSettleMs + kMinGapMs;                           // heavy at 18000 (expires 38000 too: pick distinct)
        CHECK(q.Tick(t2, Playing()).size() == 1 && q.ActiveTimers() == 1);
        CHECK(q.Tick(t2 + 19999, Playing()).empty());
        auto u = q.Tick(t2 + 20000, Playing());
        CHECK(u.size() == 1 && u[0].command == "(physics_constants_reset)" && u[0].kind == Action::Undo);
        auto re = q.Tick(t2 + 30000, Playing());                       // QA M1: neutral state is re-asserted while nothing runs
        CHECK(re.size() == 1 && re[0].kind == Action::Reassert && re[0].command == "(physics_constants_reset)");
        CHECK(q.Tick(t2 + 30000 + kReassertMs - 1, Playing()).empty());
        CHECK(q.Tick(t2 + 30000 + kReassertMs, Playing()).size() == 1);
    }

    // QA M1: re-assert only after a gravity/speed effect, only for that mission, once more in the next mission
    {
        EffectQueue q;
        q.Tick(0, Playing());
        for (uint64_t t = kSettleMs; t < kSettleMs + 60000; t += 1000) {  // no state effect ever: nothing is re-asserted
            CHECK(q.Tick(t, Playing()).empty());
        }
        EffectQueue s;
        s.Tick(0, Playing());
        s.Add(81105, 1);                                                // slow motion, trap, 15 s
        uint64_t t = kTrapSettleMs;
        CHECK(s.Tick(t, Playing()).size() == 1);
        CHECK(s.Tick(t + 14999, Playing()).empty());                    // while active: nothing
        auto u = s.Tick(t + 15000, Playing());
        CHECK(u.size() == 1 && u[0].kind == Action::Undo && u[0].command == "(game_speed 1)");
        CHECK(s.Tick(t + 15000 + kReassertMs - 1, Playing()).empty());
        auto re = s.Tick(t + 15000 + kReassertMs, Playing());
        CHECK(re.size() == 1 && re[0].kind == Action::Reassert && re[0].command == "(game_speed 1)");
        s.OnMissionLoad(t + 40000);                                     // next mission: stops, then one cleanup after the settle
        CHECK(s.Tick(t + 40000 + 1000, Playing()).empty());
        auto c = s.Tick(t + 40000 + kSettleMs, Playing());
        CHECK(c.size() == 1 && c[0].kind == Action::Cleanup && c[0].command == "(game_speed 1)");
        CHECK(s.Tick(t + 40000 + kSettleMs + 3 * kReassertMs, Playing()).empty());  // not re-asserted again in that mission
        CHECK(s.Tick(0 + 1, Gate()).empty());                           // never in the menu
    }

    // QA m1/m2: a failed post changes nothing: apply is retried (front of the queue, gap not consumed), an undo is kept
    {
        EffectQueue q;
        q.Tick(0, Playing());
        q.Add(81012, 1); q.Add(81001, 2);
        auto a = q.Tick(kSettleMs, Playing());
        CHECK(a.size() == 1 && q.ActiveTimers() == 1);
        q.Failed(a[0], kSettleMs);
        CHECK(q.ActiveTimers() == 0 && q.Pending() == 2 && !q.Applied(1));
        CHECK(q.Tick(kSettleMs + kRetryMs - 1, Playing()).empty());
        auto b = q.Tick(kSettleMs + kRetryMs, Playing());
        CHECK(b.size() == 1 && b[0].index == 1);                        // same item again, still first, no 10 s gap wait
        q.Tick(kSettleMs + kRetryMs, Playing());
        auto e = q.Tick(kSettleMs + kRetryMs + 30000, Playing());
        CHECK(e.size() >= 1 && e[0].kind == Action::Undo);
        q.Failed(e[0], kSettleMs + kRetryMs + 30000);                   // undo failed: kept and re-sent
        CHECK(q.DeferredUndos() == 1);
        auto again = q.Tick(kSettleMs + kRetryMs + 30000 + kRetryMs, Playing());
        bool found = false;
        for (auto& x : again) if (x.kind == Action::Cleanup && x.command == "(set cheat_infinite_ammo false)") found = true;
        CHECK(found);
    }

    // leaving the mission voids the timers; the undo runs once in the next mission after the settle delay
    {
        EffectQueue q;
        q.Tick(0, Playing());
        q.Add(81105, 1);
        q.Tick(kTrapSettleMs, Playing());
        CHECK(q.ActiveTimers() == 1);
        Gate menu;
        CHECK(q.Tick(kTrapSettleMs + 1000, menu).empty());
        CHECK(q.ActiveTimers() == 0 && q.DeferredUndos() == 1);
        CHECK(q.Tick(kTrapSettleMs + 60000, menu).empty());           // nothing while in the menu, no stale expiry
        uint64_t n = kTrapSettleMs + 70000;
        CHECK(q.Tick(n, Playing()).empty());
        auto c = q.Tick(n + kSettleMs, Playing());
        CHECK(c.size() == 1 && c[0].kind == Action::Cleanup && c[0].command == "(game_speed 1)");
        CHECK(q.Tick(n + kSettleMs + 100, Playing()).empty());
    }

    // a reload while still "in mission" (restart): timers void, settle restarts, pending stays queued
    {
        EffectQueue q;
        q.Tick(0, Playing());
        q.Add(81011, 1);
        q.Tick(kSettleMs, Playing());
        q.Add(81001, 2);
        q.OnMissionLoad(kSettleMs + 1000);
        CHECK(q.ActiveTimers() == 0 && q.DeferredUndos() == 1 && q.Pending() == 1);
        CHECK(q.Tick(kSettleMs + 2000, Playing()).empty());
        auto a = q.Tick(kSettleMs + kMinGapMs, Playing());
        CHECK(a.size() == 2 && a[0].kind == Action::Cleanup && a[1].kind == Action::Apply);
    }

    // clock skew across threads must not look like "long ago"
    {
        EffectQueue q;
        q.OnMissionLoad(5000);
        q.Add(81001, 1);
        CHECK(q.Tick(4990, Playing()).empty());
    }

    std::printf("effects tests: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
