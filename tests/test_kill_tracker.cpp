// Plain C++ tests for KillTracker. No framework; exit code != 0 on failure.
#include "kill_tracker.h"
#include "data/kill_map.h"

#include <algorithm>
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
    t["m1"] = { 1, { {1, 0}, {1, 1}, {2, 0} },
                { {"grunt", 201}, {"elite", 202} },
                { {10, "grunt"}, {11, "elite"}, {12, "warthog"}, {50, "marine"}, {60, "flood_infection"} },
                { 10, 11, 12, 99 },  // hostile countable variants; 50 = marine (friendly), 60 = flood_infection (uncountable)
                100, 50 };  // killBasis, swarmMax (m1 has infection-form data, m2 has none)
    t["m2"] = { 2, { {1, 0} }, { {"grunt", 302} }, { {10, "grunt"} }, { 10, 99 } };
    return t;
}

// Feeds `n` fresh hostile kills (variant 99: hostile, no species) and returns every ID emitted.
static Ids Feed(KillTracker& k, uint32_t firstHandle, int n, int variant = 99) {
    Ids all;
    for (int i = 0; i < n; ++i) {
        for (int64_t id : k.OnEvent(Ev(firstHandle + i, 1, 0, variant))) all.push_back(id);
    }
    return all;
}

static void TestIdFormula() {
    CHECK(MilestoneId(1, 25) == 1000000000LL + 10000000LL + 101LL * 10000 + 25);
    CHECK(MilestoneId(10, 9999) == 1000000000LL + 10000000LL + 110LL * 10000 + 9999);
    CHECK(MilestoneId(10, 9999) < 2147483647LL);
}

static SwarmEvent Sw(uint32_t unit, uint32_t ekf = 0) {
    SwarmEvent e{};
    e.ah = 1; e.unit = unit; e.var = 0xE0000000u | 60; e.ekf = ekf; e.tid = 1; e.enc = 1; e.sq = 0; e.members = 3;
    return e;
}

static Ids FeedSwarm(KillTracker& k, uint32_t firstUnit, int n) {
    Ids all;
    for (int i = 0; i < n; ++i) {
        for (int64_t id : k.OnSwarm(Sw(firstUnit + i))) all.push_back(id);
    }
    return all;
}

static void TestInfectionFormula() {
    CHECK(InfectionId(1, 25) == 1000000000LL + 10000000LL + 201LL * 10000 + 25);
    CHECK(InfectionId(10, 9999) == 1000000000LL + 10000000LL + 210LL * 10000 + 9999);
    CHECK(InfectionId(10, 9999) < 2147483647LL);
    CHECK(InfectionId(1, 1) != MilestoneId(1, 1) && InfectionId(1, 1) != MilestoneId(10, 1));
}

static void TestInfectionChecks() {
    KillTracker k(Small());
    k.SetConfig(0, false, 10);
    k.OnMissionLoad("m1");
    CHECK((FeedSwarm(k, 1, 25) == Ids{ InfectionId(1, 10), InfectionId(1, 20) }));
    CHECK(k.SwarmCount() == 25);
    CHECK(FeedSwarm(k, 1, 5).empty());                     // same unit handles: no recount
    CHECK(k.OnSwarm(Sw(500, 0x20)).empty());               // scripted unit_kill: not counted
    CHECK(k.SwarmCount() == 25);
    CHECK((FeedSwarm(k, 100, 5) == Ids{ InfectionId(1, 30) }));
    // infection forms never touch the kill count or milestones
    CHECK(k.Count() == 0);
    // a mission without swarm data counts nothing
    k.OnMissionLoad("m2");
    CHECK(FeedSwarm(k, 1, 40).empty());
    CHECK(k.SwarmCount() == 0);
    // a new run of m1 restarts the infection count
    k.OnMissionLoad("m1");
    CHECK(k.SwarmCount() == 0);
    CHECK((FeedSwarm(k, 1, 10) == Ids{ InfectionId(1, 10) }) == false);  // already emitted under this config (de-duplicated)
    // off by default and when 0
    KillTracker z(Small());
    z.OnMissionLoad("m1");
    CHECK(FeedSwarm(z, 1, 50).empty());
    z.SetConfig(25, true, 0);
    CHECK(FeedSwarm(z, 100, 50).empty());
}

static void TestInfectionSetConfigAfterKills() {
    KillTracker k(Small());
    k.OnMissionLoad("m1");
    FeedSwarm(k, 1, 30);                                   // before the slot connected
    CHECK((k.SetConfig(0, false, 10) == Ids{ InfectionId(1, 10), InfectionId(1, 20), InfectionId(1, 30) }));
    CHECK(k.SetConfig(0, false, 0).empty());
    k.ResetHistory();
    CHECK(k.SetConfig(0, false, 10).empty());
    CHECK(Feed(k, 1, 5).size() == 0);                      // milestones off: infection config does not leak
}

static void TestIntervalMilestones() {
    KillTracker k(Small());
    k.SetConfig(25, false);
    k.OnMissionLoad("m1");
    Ids got = Feed(k, 1, 60);
    CHECK((got == Ids{ MilestoneId(1, 25), MilestoneId(1, 50) }));
    CHECK(k.Count() == 60);
    CHECK(Feed(k, 100, 15) == Ids{ MilestoneId(1, 75) });
}

static void TestIntervalOneAndNone() {
    KillTracker k(Small());
    k.SetConfig(1, false);
    k.OnMissionLoad("m1");
    CHECK((Feed(k, 1, 3) == Ids{ MilestoneId(1, 1), MilestoneId(1, 2), MilestoneId(1, 3) }));
    KillTracker z(Small());
    z.SetConfig(0, false);
    z.OnMissionLoad("m1");
    CHECK(Feed(z, 1, 80).empty());
    KillTracker d(Small());  // defaults (no SetConfig): nothing emitted either
    d.OnMissionLoad("m1");
    CHECK(Feed(d, 1, 80).empty());
    KillTracker c(Small());  // out-of-range interval is clamped to 100
    c.SetConfig(500, false);
    c.OnMissionLoad("m1");
    CHECK(Feed(c, 1, 100) == Ids{ MilestoneId(1, 100) });
}

static void TestSpeciesToggle() {
    KillTracker off(Small());
    off.SetConfig(0, false);
    off.OnMissionLoad("m1");
    CHECK(off.OnEvent(Ev(1, 1, 0, 10)).empty());  // species off: nothing
    KillTracker k(Small());
    k.SetConfig(0, true);
    k.OnMissionLoad("m1");
    CHECK(k.OnEvent(Ev(1, 1, 0, 10)) == Ids{ 201 });
    CHECK(k.OnEvent(Ev(2, 1, 0, 10)).empty());       // second grunt: species only once
    CHECK(k.OnEvent(Ev(3, 1, 1, 11)) == Ids{ 202 });
    CHECK(k.OnEvent(Ev(4, 1, 1, 12)).empty());       // warthog variant has no species location
    CHECK(k.OnEvent(Ev(5, 1, 1, 99)).empty());       // hostile variant without a species location: counts, no species
    AiEvent hi = Ev(6, 2, 0, 10);
    hi.var = 0x1234000Bu;                            // only the low 16 bits are the tag index
    CHECK(k.OnEvent(hi).empty());                    // elite again
    CHECK(k.Count() == 6);
}

static void TestIgnored() {
    KillTracker k(Small());
    k.SetConfig(1, true);
    CHECK(k.OnEvent(Ev(1, 1, 0, 10)).empty());  // no mission yet
    k.OnMissionLoad("m1");
    CHECK(k.OnEvent(Ev(2, 1, 0, 10, true, 0x20)).empty());  // scripted unit_kill
    CHECK((k.OnEvent(Ev(3, 1, 0, 10, true, 0x40)) == Ids{ MilestoneId(1, 1), 201 }));  // silent kill (0x40) counts
    CHECK(k.OnEvent(Ev(4, 9, 9, 50)).empty());              // friendly variant (marine), squad irrelevant
    CHECK(k.OnEvent(Ev(5, -1, -1, 50)).empty());
    CHECK(k.OnEvent(Ev(4, 1, 0, 50, false)).empty());       // friendly ERASE, even inside a hostile squad
    CHECK(k.OnEvent(Ev(6, 1, 0, 777)).empty());             // unknown variant: not counted
    CHECK(k.Count() == 1);
    k.OnMissionLoad("nope");  // unknown mission -> nothing
    CHECK(k.OnEvent(Ev(6, 1, 0, 11)).empty());
    k.OnMissionLoad("");
    CHECK(k.OnEvent(Ev(7, 1, 0, 11)).empty());
}

// ADR-003 amendment 2: kills are counted by the actor's variant, never by the squad it dies in.
static void TestCountByActor() {
    KillTracker k(Small());
    k.SetConfig(1, true);
    k.OnMissionLoad("m1");
    // an actor migrated into an empty destination squad (7,7: not in the hostile squad list) still counts
    CHECK((k.OnEvent(Ev(1, 7, 7, 10)) == Ids{ MilestoneId(1, 1), 201 }));
    // enc/sq = -1 (e.g. a50 hunters) counts
    CHECK((k.OnEvent(Ev(2, -1, -1, 11)) == Ids{ MilestoneId(1, 2), 202 }));
    CHECK(k.OnEvent(Ev(3, -1, -1, 12, false)) == Ids{ MilestoneId(1, 3) });  // erase with enc/sq = -1 counts
    // a friendly variant never counts, even sitting in a hostile squad (1,0), dying or erased
    CHECK(k.OnEvent(Ev(4, 1, 0, 50)).empty());
    CHECK(k.OnEvent(Ev(5, 1, 0, 50, false)).empty());
    // flood_infection (uncountable): not in the hostile list
    CHECK(k.OnEvent(Ev(6, 1, 0, 60)).empty());
    // scripted unit_kill of a hostile variant does not count
    CHECK(k.OnEvent(Ev(7, 1, 0, 10, true, 0x20)).empty());
    CHECK(k.Count() == 3);
    // an unknown variant is never counted
    CHECK(k.OnEvent(Ev(8, 1, 0, 4242)).empty());
    CHECK(k.Count() == 3);
}

static void TestDedupe() {
    KillTracker k(Small());
    k.SetConfig(2, true);
    k.OnMissionLoad("m1");
    CHECK(k.OnEvent(Ev(7, 1, 0, 10)) == Ids{ 201 });  // first grunt
    CHECK(k.OnEvent(Ev(7, 1, 0, 10)).empty());        // same actor handle: no second count
    CHECK(k.Count() == 1);
    CHECK((k.OnEvent(Ev(8, 1, 1, 11)) == Ids{ MilestoneId(1, 2), 202 }));  // 2nd kill: milestone, first elite
    CHECK(k.OnEvent(Ev(8, 1, 1, 11)).empty());
}

// QUESTIONS #9: a script despawning a hostile actor (ERASE) counts as a kill.
static void TestEraseCounts() {
    KillTracker k(Small());
    k.SetConfig(3, true);
    k.OnMissionLoad("m1");
    CHECK(k.OnEvent(Ev(1, 1, 0, 11, false)) == Ids{ 202 });       // erased elite: species
    CHECK(k.OnEvent(Ev(2, 1, 0, 12, false, 0x20)).empty());      // kill flags are ignored on ERASE: still counts
    CHECK(k.Count() == 2);
    CHECK(k.OnEvent(Ev(3, 1, 0, 12, false)) == Ids{ MilestoneId(1, 3) });  // 3rd: milestone, from erases alone
    CHECK(k.OnEvent(Ev(3, 1, 0, 12)).empty());                   // the same actor dying too: no double count
    CHECK(k.OnEvent(Ev(4, 1, 0, 12)).empty());
    CHECK(k.OnEvent(Ev(4, 1, 0, 12, false)).empty());            // ...or erased after dying
    CHECK(k.Count() == 4);
}

static void TestMissionSwitchResetsCount() {
    KillTracker k(Small());
    k.SetConfig(2, true);
    k.OnMissionLoad("m1");
    CHECK(Feed(k, 1, 1) == Ids{});
    CHECK(k.Count() == 1);
    k.OnMissionLoad("m2");  // new run: count resets, so m2's 2nd kill is its own milestone 2
    CHECK(k.Count() == 0);
    CHECK(k.OnEvent(Ev(1, 1, 0, 10)) == Ids{ 302 });  // same handle value is a fresh actor in a new run
    CHECK(k.OnEvent(Ev(2, 1, 0, 99)) == Ids{ MilestoneId(2, 2) });
    k.OnMissionLoad("m1");  // restart m1: counting restarts from zero
    CHECK(k.Count() == 0);
    CHECK(Feed(k, 1, 4) == (Ids{ MilestoneId(1, 2), MilestoneId(1, 4) }));
    k.OnMissionLoad("m1");  // replay: nothing is returned twice
    CHECK(Feed(k, 1, 4).empty());
}

// Slot options arrive after kills may have started: SetConfig re-evaluates the whole history.
static void TestSetConfigAfterKills() {
    KillTracker k(Small());
    k.OnMissionLoad("m1");
    CHECK(k.OnEvent(Ev(1, 1, 0, 10)).empty());
    CHECK(k.OnEvent(Ev(2, 1, 1, 11)).empty());
    Feed(k, 10, 5);
    CHECK(k.Count() == 7);
    k.OnMissionLoad("m2");
    CHECK(k.OnEvent(Ev(1, 1, 0, 10)).empty());
    CHECK(k.OnEvent(Ev(2, 1, 0, 99)).empty());
    Ids got = k.SetConfig(2, true);
    std::sort(got.begin(), got.end());
    CHECK((got == Ids{ 201, 202, 302, MilestoneId(1, 2), MilestoneId(1, 4), MilestoneId(1, 6), MilestoneId(2, 2) }));
    CHECK(k.SetConfig(2, true).size() == got.size());     // a repeat re-evaluates everything (caller de-dups)
    CHECK(k.OnEvent(Ev(3, 1, 0, 99)).empty());           // m2 count 3: not a multiple
    CHECK(k.OnEvent(Ev(4, 1, 0, 99)) == Ids{ MilestoneId(2, 4) });
    Ids other = k.SetConfig(5, false);                    // a different seed: only 5-multiples, no species
    CHECK(other == Ids{ MilestoneId(1, 5) });             // peaks are 7 (m1) and 4 (m2)
    CHECK(k.SetConfig(0, false).empty());
}

// QA 5.4 m2: a different seed connecting must not be credited with kills made for the old one.
static void TestResetHistory() {
    KillTracker k(Small());
    k.OnMissionLoad("m1");
    Feed(k, 1, 4);
    k.OnEvent(Ev(50, 1, 0, 10));                         // a grunt: species 201 seen
    CHECK(k.SetConfig(2, true).size() == 3);             // 2, 4 and species
    k.ResetHistory();
    CHECK(k.SetConfig(2, true).empty());                 // new seed: nothing from before
    CHECK(Feed(k, 100, 1) == Ids{ MilestoneId(1, 6) });  // the current run's count carries on
}

static void TestRealA10Table() {
    const KillTables& t = GetKillTables();
    CHECK(t.size() == 10);
    auto it = t.find("a10");
    CHECK(it != t.end());
    if (it == t.end()) return;
    const MissionKillTable& a = it->second;
    CHECK(!a.squads.empty() && a.missionNo == 1 && a.species.size() == 2 && !a.variants.empty());
    for (const auto& kv : t) CHECK(!kv.second.hostileVariants.empty());
    auto isHostile = [&](int v) { for (int h : a.hostileVariants) if (h == v) return true; return false; };
    CHECK(isHostile(2655) && isHostile(2419));        // grunt, elite
    CHECK(!isHostile(2413) && !isHostile(2653));      // crewmen are friendly
    int expectNo = 0;
    for (const char* code : { "a10", "a30", "a50", "b30", "b40", "c10", "c20", "c40", "d20", "d40" }) {
        CHECK(t.at(code).missionNo == ++expectNo);
    }
    KillTracker k(GetKillTables());
    k.SetConfig(25, true);
    k.OnMissionLoad("a10");
    // a10 squad (1,0) is hostile; variant 2655 = grunt, 2419 = elite.
    Ids got;
    uint32_t ah = 1;
    auto feed = [&](int variant) { for (int64_t id : k.OnEvent(Ev(ah++, 1, 0, variant))) got.push_back(id); };
    feed(2655);
    CHECK(got == Ids{ 1010019004 });  // first grunt
    feed(2419);
    CHECK(got.size() == 2 && got[1] == 1010019003);  // first elite
    while (k.Count() < 50) feed(2655);
    CHECK((got == Ids{ 1010019004, 1010019003, MilestoneId(1, 25), MilestoneId(1, 50) }));
    CHECK(MilestoneId(1, 25) == 1011010025LL);
}

int main() {
    TestIdFormula();
    TestInfectionFormula();
    TestInfectionChecks();
    TestInfectionSetConfigAfterKills();
    TestIntervalMilestones();
    TestIntervalOneAndNone();
    TestSpeciesToggle();
    TestIgnored();
    TestCountByActor();
    TestDedupe();
    TestEraseCounts();
    TestMissionSwitchResetsCount();
    TestSetConfigAfterKills();
    TestResetHistory();
    TestRealA10Table();
    std::printf("kill-tracker tests: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
