// Plain C++ tests for KillTracker. No framework; exit code != 0 on failure.
#include "kill_tracker.h"
#include "data/kill_map.h"

#include <cstdio>

using namespace haloap;

static int g_checks = 0;
static int g_failures = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

using Ids = std::vector<int64_t>;

static AiEvent Ev(uint32_t ah, int enc, int sq, int variant, bool kill = true, uint32_t ekf = 0) {
    AiEvent e{};
    e.kill = kill; e.ah = ah; e.unit = 0x1000 + ah; e.enc = enc; e.sq = sq; e.pl = 0;
    e.var = 0xE0000000u | static_cast<uint32_t>(variant); e.flags = 0; e.ekf = ekf;
    e.living = 0; e.total = 1; e.el = 1; e.et = 1; e.tid = 1;
    return e;
}

static KillTables Small() {
    KillTables t;
    t["m1"] = { { {1, 0}, {1, 1}, {2, 0} },
                { {3, 101}, {5, 102} },
                { {"grunt", 201}, {"elite", 202} },
                { {10, "grunt"}, {11, "elite"}, {12, "warthog"} } };
    t["m2"] = { { {1, 0} }, { {2, 301} }, { {"grunt", 302} }, { {10, "grunt"} } };
    return t;
}

static void TestThresholds() {
    KillTracker k(Small());
    k.OnMissionLoad("m1");
    CHECK(k.OnEvent(Ev(1, 1, 0, 12)).empty());
    CHECK(k.OnEvent(Ev(2, 1, 1, 12)).empty());
    CHECK(k.OnEvent(Ev(3, 2, 0, 12)) == Ids{ 101 });
    CHECK(k.OnEvent(Ev(4, 2, 0, 12)).empty());
    CHECK(k.OnEvent(Ev(5, 2, 0, 12)) == Ids{ 102 });
    CHECK(k.OnEvent(Ev(6, 2, 0, 12)).empty());
    CHECK(k.Count() == 6);
}

static void TestIgnored() {
    KillTracker k(Small());
    CHECK(k.OnEvent(Ev(1, 1, 0, 10)).empty());  // no mission yet
    k.OnMissionLoad("m1");
    CHECK(k.OnEvent(Ev(1, 1, 0, 10, false)).empty());       // ERASE
    CHECK(k.OnEvent(Ev(2, 1, 0, 10, true, 0x20)).empty());  // scripted unit_kill
    CHECK(k.OnEvent(Ev(3, 1, 0, 10, true, 0x40)) == Ids{ 201 });  // silent kill (0x40) still counts
    CHECK(k.OnEvent(Ev(4, 9, 9, 11)).empty());              // non-hostile squad
    CHECK(k.OnEvent(Ev(5, -1, -1, 11)).empty());
    CHECK(k.Count() == 1);
    k.OnMissionLoad("nope");  // unknown mission -> nothing
    CHECK(k.OnEvent(Ev(6, 1, 0, 11)).empty());
    k.OnMissionLoad("");
    CHECK(k.OnEvent(Ev(7, 1, 0, 11)).empty());
}

static void TestDedupeAndSpecies() {
    KillTracker k(Small());
    k.OnMissionLoad("m1");
    CHECK(k.OnEvent(Ev(7, 1, 0, 10)) == Ids{ 201 });  // first grunt
    CHECK(k.OnEvent(Ev(7, 1, 0, 10)).empty());        // same actor handle
    CHECK(k.Count() == 1);
    CHECK(k.OnEvent(Ev(8, 1, 0, 10)).empty());        // second grunt: species only once
    CHECK(k.OnEvent(Ev(9, 1, 1, 11)) == (Ids{ 101, 202 }));  // 3rd kill: milestone, then first elite
    CHECK(k.OnEvent(Ev(10, 1, 1, 11)).empty());
}

static void TestVariantMatching() {
    KillTracker k(Small());
    k.OnMissionLoad("m1");
    CHECK(k.OnEvent(Ev(1, 1, 0, 11)) == Ids{ 202 });
    CHECK(k.OnEvent(Ev(2, 1, 0, 12)).empty());        // warthog variant has no species location
    CHECK(k.OnEvent(Ev(3, 1, 0, 99)) == Ids{ 101 });  // unknown variant still counts as a kill
    AiEvent hi = Ev(4, 1, 0, 10);
    hi.var = 0x1234000Au;                             // only the low 16 bits are the tag index
    CHECK(k.OnEvent(hi) == Ids{ 201 });
}

static void TestMissionSwitchAndNoRepeat() {
    KillTracker k(Small());
    k.OnMissionLoad("m1");
    k.OnEvent(Ev(1, 1, 0, 99));
    k.OnEvent(Ev(2, 1, 0, 99));
    CHECK(k.Count() == 2);
    k.OnMissionLoad("m2");  // new run: count resets
    CHECK(k.Count() == 0);
    CHECK(k.OnEvent(Ev(1, 1, 0, 10)) == Ids{ 302 });  // same handle value is a fresh actor in a new run
    CHECK(k.OnEvent(Ev(2, 1, 0, 99)) == Ids{ 301 });
    k.OnMissionLoad("m1");  // restart m1: counting restarts from zero
    CHECK(k.Count() == 0);
    Ids all;
    for (uint32_t i = 1; i <= 8; ++i) {
        for (int64_t id : k.OnEvent(Ev(i, 2, 0, 10))) all.push_back(id);
    }
    CHECK(all == (Ids{ 201, 101, 102 }));
    k.OnMissionLoad("m1");  // replay: nothing is ever returned twice
    for (uint32_t i = 1; i <= 8; ++i) CHECK(k.OnEvent(Ev(i, 2, 0, 10)).empty());
    CHECK(k.Count() == 8);
}

static void TestRealA10Table() {
    const KillTables& t = GetKillTables();
    CHECK(t.size() == 10);
    auto it = t.find("a10");
    CHECK(it != t.end());
    if (it == t.end()) return;
    const MissionKillTable& a = it->second;
    CHECK(!a.squads.empty() && a.milestones.size() == 3 && a.species.size() == 2 && !a.variants.empty());
    KillTracker k(GetKillTables());
    k.OnMissionLoad("a10");
    // a10 squad (1,0) is hostile; variant 2655 = grunt, 2419 = elite.
    Ids got;
    uint32_t ah = 1;
    auto feed = [&](int variant) { for (int64_t id : k.OnEvent(Ev(ah++, 1, 0, variant))) got.push_back(id); };
    feed(2655);
    CHECK(got == Ids{ 1010019004 });  // first grunt
    feed(2419);
    CHECK(got == (Ids{ 1010019004, 1010019003 }));  // first elite
    while (k.Count() < 10) feed(2655);
    CHECK(got.size() == 3 && got.back() == 1010019000);  // 10 kills
    while (k.Count() < 50) feed(2655);
    CHECK((got == Ids{ 1010019004, 1010019003, 1010019000, 1010019001, 1010019002 }));
}

int main() {
    TestThresholds();
    TestIgnored();
    TestDedupeAndSpecies();
    TestVariantMatching();
    TestMissionSwitchAndNoRepeat();
    TestRealA10Table();
    std::printf("kill-tracker tests: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
