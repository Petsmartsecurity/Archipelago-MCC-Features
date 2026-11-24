// Plain C++ tests for GameProgress (per-game mission / goal logic) and the generated Halo 2 tables.
#include "game_progress.h"
#include "kill_tracker.h"
#include "data/h2_map.h"
#include "data/kill_map.h"
#include "data/mission_map.h"

#include <cstdio>

using namespace haloap;
using nlohmann::json;

static int g_checks = 0;
static int g_failures = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

using Msgs = std::vector<std::string>;
using Flags = std::set<std::string>;

static void TestTables() {
    // CE ids agree with the old mission map; every CE code resolves to index order a10..d40
    for (const auto& [code, id] : GetMissionIdMap()) {
        auto m = GameProgress::ByCode(code);
        CHECK(m && m->game == Game::CE && m->id == id && GameProgress::ByLocation(id)->index == m->index);
    }
    CHECK(GameProgress::ByCode("a10")->index == 0 && GameProgress::ByCode("d40")->index == 9);
    CHECK(GameProgress::ByCode("d40")->id == 110000);
    // H2: 14 missions n = 2..15, The Heretic is never a check
    static_assert(sizeof(kH2Missions) / sizeof(kH2Missions[0]) == 14);
    auto g = GameProgress::ByCode("08b_deltacontrol");
    CHECK(g && g->game == Game::H2 && g->index == 15 && g->id == 215000);
    CHECK(!GameProgress::ByCode("00a_introduction"));
    CHECK(GameProgress::IsH2Code("00a_introduction") && GameProgress::IsH2Code("03a_oldmombasa"));
    CHECK(!GameProgress::IsH2Code("a10") && !GameProgress::IsH2Code("") && !GameProgress::IsH2Code("zzz"));
    CHECK(!GameProgress::ByLocation(201000) && !GameProgress::ByLocation(101011) && !GameProgress::ByLocation(216000));
    CHECK(GameProgress::ByLocation(205000)->index == 5);
    // chapters: (code, title index) -> id; 03b's title 0,1,2 are one chapter
    CHECK(GameProgress::H2Chapter("01a_tutorial", 0)->id == 202011);
    CHECK(GameProgress::H2Chapter("03b_newmombasa", 0)->id == 205011);
    CHECK(GameProgress::H2Chapter("03b_newmombasa", 1)->id == 205011);
    CHECK(GameProgress::H2Chapter("03b_newmombasa", 2)->id == 205011);
    CHECK(GameProgress::H2Chapter("03b_newmombasa", 3)->id == 205012);
    CHECK(GameProgress::H2Chapter("03b_newmombasa", 4)->id == 205013);
    CHECK(!GameProgress::H2Chapter("03b_newmombasa", 5));
    CHECK(!GameProgress::H2Chapter("05b_deltatowers", 2));      // excluded title
    CHECK(!GameProgress::H2Chapter("00a_introduction", 0));
    CHECK(!GameProgress::H2Chapter("a10", 0));
}

static void TestOldSeedAndCeOnly() {
    GameProgress p;  // before slot connect: today's defaults
    CHECK(!p.Configured() && p.Wants(Game::CE) && p.Wants(Game::H2));
    Msgs def = p.DllMessages({});
    CHECK(def == (Msgs{ "COMPLETED:", "FINAL_MISSION:9" }));
    CHECK(!p.GoalReached({ "CE" }));  // never before the config is known

    // old seed: no ce_enabled key, only final_mission
    GameProgress o;
    o.Configure(json{ {"final_mission", "Keyes"}, {"skullsanity", 1} });
    CHECK(o.Enabled(Game::CE) && !o.Enabled(Game::H2));
    CHECK(o.DllMessages({ 101000, 103000, 109000, 1010010000LL }) == (Msgs{ "COMPLETED:0,2,8", "FINAL_MISSION:8" }));
    CHECK(!o.GoalReached({}) && o.GoalReached({ "CE" }) && !o.GoalReached({ "H2" }));
    // old seed with no final at all: The Maw, as before; empty / null slot data likewise
    GameProgress o2;
    o2.Configure(json::object());
    CHECK(o2.DllMessages({})[1] == "FINAL_MISSION:9");
    GameProgress o3;
    o3.Configure(json());
    CHECK(o3.DllMessages({})[1] == "FINAL_MISSION:9" && o3.Enabled(Game::CE));
    // old seed with an unknown final name: none (no operator[] insert of index 0)
    GameProgress o4;
    o4.Configure(json{ {"final_mission", "Pillar"} });
    CHECK(o4.DllMessages({})[1] == "FINAL_MISSION:-1" && !o4.GoalReached({ "CE" }));
    CHECK(!o4.OnMissionComplete(*GameProgress::ByCode("a10")));  // The Pillar of Autumn is not the goal

    // CE-only seed from the new apworld: byte-identical CE lines, no H2 lines
    GameProgress c;
    c.Configure(json{ {"ce_enabled", 1}, {"h2_enabled", 0}, {"final_mission", "The Maw"}, {"h2_final_mission", ""} });
    CHECK(c.DllMessages({ 102000 }) == (Msgs{ "COMPLETED:1", "FINAL_MISSION:9" }));
    CHECK(!c.OnMissionComplete(*GameProgress::ByCode("d20")));
    CHECK(c.OnMissionComplete(*GameProgress::ByCode("d40")) == Game::CE);
    CHECK(c.GoalReached({ "CE" }));
    // CE on with a missing / empty final (new seed) = none: goal unreachable
    GameProgress n;
    n.Configure(json{ {"ce_enabled", 1} });
    CHECK(n.DllMessages({})[1] == "FINAL_MISSION:-1" && !n.GoalReached({ "CE" }));
    CHECK(!n.OnMissionComplete(*GameProgress::ByCode("d40")));
}

static void TestH2() {
    // H2 only: FINAL_MISSION:-1 for the DLL, H2 lines present, CE completions ignored
    GameProgress h;
    h.Configure(json{ {"ce_enabled", 0}, {"h2_enabled", 1}, {"final_mission", ""}, {"h2_final_mission", "The Great Journey"} });
    CHECK(!h.Enabled(Game::CE) && h.Enabled(Game::H2) && !h.Wants(Game::CE));
    CHECK(h.DllMessages({ 202000, 215000, 212000, 205011 }) ==
        (Msgs{ "COMPLETED:", "FINAL_MISSION:-1", "H2_COMPLETED:2,12,15", "H2_FINAL:15" }));
    CHECK(!h.OnMissionComplete(*GameProgress::ByCode("d40")));              // CE final of a CE-off seed: nothing
    CHECK(!h.OnMissionComplete(*GameProgress::ByCode("07a_highcharity")));
    CHECK(h.OnMissionComplete(*GameProgress::ByCode("08b_deltacontrol")) == Game::H2);
    CHECK(h.GoalReached({ "H2" }) && !h.GoalReached({ "CE" }) && !h.GoalReached({}));
    CHECK(h.GoalReached({ "CE", "H2" }));  // a stray CE flag does not matter

    // both games: goal only when both finals are done
    GameProgress b;
    b.Configure(json{ {"ce_enabled", 1}, {"h2_enabled", 1}, {"final_mission", "The Maw"}, {"h2_final_mission", "High Charity"} });
    CHECK(b.DllMessages({ 110000, 214000 }) ==
        (Msgs{ "COMPLETED:9", "FINAL_MISSION:9", "H2_COMPLETED:14", "H2_FINAL:14" }));
    CHECK(b.OnMissionComplete(*GameProgress::ByCode("d40")) == Game::CE);
    CHECK(!b.GoalReached({ "CE" }) && !b.GoalReached({ "H2" }) && b.GoalReached({ "CE", "H2" }));
    CHECK(b.OnMissionComplete(*GameProgress::ByCode("07b_forerunnership")) == Game::H2);
    CHECK(!b.OnMissionComplete(*GameProgress::ByCode("08b_deltacontrol")));  // not this seed's H2 final

    // H2 on with an unknown / Heretic / empty final: none, H2_FINAL:0, goal unreachable even if CE is done
    for (const char* name : { "", "The Heretic", "Nope" }) {
        GameProgress u;
        u.Configure(json{ {"ce_enabled", 1}, {"h2_enabled", 1}, {"final_mission", "The Maw"}, {"h2_final_mission", name} });
        CHECK(u.DllMessages({})[3] == "H2_FINAL:0" && !u.GoalReached({ "CE", "H2" }));
    }
    // booleans and wrong types
    GameProgress t;
    t.Configure(json{ {"ce_enabled", false}, {"h2_enabled", true}, {"h2_final_mission", "Regret"}, {"final_mission", 5} });
    CHECK(!t.Enabled(Game::CE) && t.Enabled(Game::H2) && t.DllMessages({})[3] == "H2_FINAL:9");
    // both games off: no goal
    GameProgress z;
    z.Configure(json{ {"ce_enabled", 0}, {"h2_enabled", 0} });
    CHECK(!z.GoalReached({ "CE", "H2" }));
}

static void TestEarlyCompletions() {
    // finals completed before the slot connected still count once the config says they were the finals
    GameProgress p;
    CHECK(!p.OnMissionComplete(*GameProgress::ByCode("d40")));
    CHECK(!p.OnMissionComplete(*GameProgress::ByCode("08b_deltacontrol")));
    CHECK(!p.OnMissionComplete(*GameProgress::ByCode("a10")));
    auto done = p.Configure(json{ {"ce_enabled", 1}, {"h2_enabled", 1}, {"final_mission", "The Maw"},
                                  {"h2_final_mission", "The Great Journey"} });
    CHECK(done.size() == 2 && done[0] == Game::CE && done[1] == Game::H2);
    GameProgress q;   // a different config: only what is a final there counts
    q.OnMissionComplete(*GameProgress::ByCode("d40"));
    q.OnMissionComplete(*GameProgress::ByCode("08b_deltacontrol"));
    done = q.Configure(json{ {"ce_enabled", 0}, {"h2_enabled", 1}, {"h2_final_mission", "The Great Journey"} });
    CHECK(done.size() == 1 && done[0] == Game::H2);
    CHECK(q.Configure(json::object()).empty());  // history was consumed
}

static void TestKillTrackerIgnoresH2() {
    KillTracker k(GetKillTables());
    k.SetConfig(1, true);
    AiEvent e{};
    e.kill = true; e.ah = 1; e.var = 0xE0000000u | 2419; e.living = 0; e.total = 1; e.el = 1; e.et = 1;
    k.OnMissionLoad("a10");
    CHECK(!k.OnEvent(e).empty());
    // what the bridge does for an H2 mission: tracker reset with an empty code, nothing counts
    const std::string h2 = "03a_oldmombasa";
    k.OnMissionLoad(GameProgress::IsH2Code(h2) ? "" : h2);
    e.ah = 2;
    CHECK(k.OnEvent(e).empty() && k.Count() == 0);
    k.OnMissionLoad(h2);  // even handed the code directly: no CE table
    CHECK(k.OnEvent(e).empty() && k.Count() == 0);
}

static void TestGameOfLocation() {
    CHECK(GameProgress::GameOfLocation(101000) == Game::CE && GameProgress::GameOfLocation(110011) == Game::CE);
    CHECK(GameProgress::GameOfLocation(199999) == Game::CE && GameProgress::GameOfLocation(100000) == Game::CE);
    CHECK(GameProgress::GameOfLocation(202000) == Game::H2 && GameProgress::GameOfLocation(215013) == Game::H2);
    CHECK(GameProgress::GameOfLocation(299999) == Game::H2);
    CHECK(!GameProgress::GameOfLocation(300000) && !GameProgress::GameOfLocation(699999));  // H3.. reserved, not known yet
    CHECK(!GameProgress::GameOfLocation(90001) && !GameProgress::GameOfLocation(81001) && !GameProgress::GameOfLocation(1));  // items
    CHECK(!GameProgress::GameOfLocation(0) && !GameProgress::GameOfLocation(-5));
    // kill / species / infection IDs: 1e9 + game*1e7 + (100|200 + mission)*1e4 + k
    CHECK(GameProgress::GameOfLocation(1010010000LL + 1) == Game::CE);        // kill milestone, mission 1
    CHECK(GameProgress::GameOfLocation(1'010'000'000LL) == Game::CE && GameProgress::GameOfLocation(1'019'999'999LL) == Game::CE);
    CHECK(GameProgress::GameOfLocation(1'020'000'000LL) == Game::H2 && GameProgress::GameOfLocation(1'029'999'999LL) == Game::H2);
    CHECK(!GameProgress::GameOfLocation(1'000'000'000LL) && !GameProgress::GameOfLocation(1'009'999'999LL));
    CHECK(!GameProgress::GameOfLocation(1'030'000'000LL) && !GameProgress::GameOfLocation(1'070'000'000LL));
    // agrees with the real kill IDs the launcher produces
    CHECK(GameProgress::GameOfLocation(MilestoneId(10, 7)) == Game::CE && GameProgress::GameOfLocation(InfectionId(3, 2)) == Game::CE);
}

static const std::set<int64_t> kMissing = { 101000, 102011, 1'010'110'005LL, 202000, 215011, 1'020'110'001LL, 1, 300001 };

static json Both(int mode, const char* goalGame = nullptr) {
    json j = { {"ce_enabled", 1}, {"h2_enabled", 1}, {"final_mission", "The Maw"}, {"h2_final_mission", "The Great Journey"} };
    if (mode >= 0) j["goal_mode"] = mode;
    if (goalGame) j["goal_game"] = goalGame;
    if (goalGame && mode == 1) { (std::string(goalGame) == "CE" ? j["h2_final_mission"] : j["final_mission"]) = ""; }
    return j;
}

static void TestGoalModes() {
    // per_game, both games: the first final releases that game's missing locations only; GOAL when both are flagged
    for (int mode : { -1, 0 }) {   // -1 = key absent (older seed)
        GameProgress p;
        p.Configure(Both(mode));
        CHECK(p.Mode() == GoalMode::PerGame);
        auto d = p.Decide({}, kMissing);
        CHECK(!d.sendGoal && d.release.empty());
        d = p.Decide({ "CE" }, kMissing);
        CHECK(!d.sendGoal && d.release == (std::vector<int64_t>{ 101000, 102011, 1'010'110'005LL }));
        d = p.Decide({ "H2" }, kMissing);
        CHECK(!d.sendGoal && d.release == (std::vector<int64_t>{ 202000, 215011, 1'020'110'001LL }));
        d = p.Decide({ "CE", "H2" }, kMissing);
        CHECK(d.sendGoal && d.release.empty());
        // idempotent: nothing missing = nothing to send; same input = same answer
        CHECK(p.Decide({ "CE" }, {}).release.empty());
        CHECK(p.Decide({ "CE" }, kMissing).release == p.Decide({ "CE" }, kMissing).release);
    }
    // per_game, one game: never releases, GOAL exactly as before
    GameProgress c;
    c.Configure(json{ {"ce_enabled", 1}, {"h2_enabled", 0}, {"final_mission", "The Maw"}, {"h2_final_mission", ""}, {"goal_mode", 0} });
    CHECK(c.Decide({}, kMissing).release.empty() && !c.Decide({}, kMissing).sendGoal);
    auto cd = c.Decide({ "CE" }, kMissing);
    CHECK(cd.sendGoal && cd.release.empty());
    GameProgress old;   // old seed, no keys at all
    old.Configure(json{ {"final_mission", "Keyes"} });
    CHECK(old.Decide({ "CE" }, kMissing).sendGoal && old.Decide({ "CE" }, kMissing).release.empty());
    GameProgress h;
    h.Configure(json{ {"ce_enabled", 0}, {"h2_enabled", 1}, {"h2_final_mission", "Regret"}, {"goal_mode", 0} });
    CHECK(h.Decide({ "H2" }, kMissing).sendGoal && h.Decide({ "H2" }, kMissing).release.empty());
    // one_final, one game: identical to per_game with one game (even if goal_game is absent or wrong)
    for (const char* gg : { (const char*)nullptr, "CE", "H2" }) {
        GameProgress o;
        json j = { {"ce_enabled", 1}, {"h2_enabled", 0}, {"final_mission", "The Maw"}, {"h2_final_mission", ""}, {"goal_mode", 1} };
        if (gg) j["goal_game"] = gg;
        o.Configure(j);
        CHECK(o.DllMessages({}) == (Msgs{ "COMPLETED:", "FINAL_MISSION:9" }));
        auto od = o.Decide({}, kMissing);
        CHECK(!od.sendGoal && od.release.empty());
        od = o.Decide({ "CE" }, kMissing);
        CHECK(od.sendGoal && od.release.empty());
        CHECK(o.OnMissionComplete(*GameProgress::ByCode("d40")) == Game::CE);
    }
}

static void TestOneFinal() {
    // CE is the goal game: the Maw wins; the Halo 2 final is an ordinary mission (no flag, no release, no DLL final)
    GameProgress ce;
    ce.Configure(Both(1, "CE"));
    CHECK(ce.Mode() == GoalMode::OneFinal);
    CHECK(ce.OnMissionComplete(*GameProgress::ByCode("d40")) == Game::CE);
    CHECK(!ce.OnMissionComplete(*GameProgress::ByCode("08b_deltacontrol")));
    CHECK(ce.DllMessages({ 215000 }) == (Msgs{ "COMPLETED:", "FINAL_MISSION:9", "H2_COMPLETED:15", "H2_FINAL:0" }));
    CHECK(!ce.GoalReached({}) && !ce.GoalReached({ "H2" }) && ce.GoalReached({ "CE" }));
    auto d = ce.Decide({ "H2" }, kMissing);       // a stray H2 flag changes nothing
    CHECK(!d.sendGoal && d.release.empty());
    d = ce.Decide({ "CE" }, kMissing);            // winning sends GOAL only, never a release
    CHECK(d.sendGoal && d.release.empty());
    // H2 is the goal game: the Maw is ordinary (DLL gets FINAL_MISSION:-1 so it unlocks by its item)
    GameProgress h2;
    h2.Configure(Both(1, "H2"));
    CHECK(!h2.OnMissionComplete(*GameProgress::ByCode("d40")));
    CHECK(h2.OnMissionComplete(*GameProgress::ByCode("08b_deltacontrol")) == Game::H2);
    CHECK(h2.DllMessages({})[1] == "FINAL_MISSION:-1" && h2.DllMessages({})[3] == "H2_FINAL:15");
    CHECK(!h2.GoalReached({ "CE" }) && h2.GoalReached({ "H2" }) && !h2.GoalReached({ "CE" }));
    CHECK(h2.Decide({ "CE" }, kMissing).release.empty());
    // even if slot_data still named both finals, the non-goal game's final does not count
    json both = Both(1, "H2");
    both["final_mission"] = "The Maw";
    GameProgress h2b;
    h2b.Configure(both);
    CHECK(!h2b.OnMissionComplete(*GameProgress::ByCode("d40")) && h2b.DllMessages({})[1] == "FINAL_MISSION:-1");
    // both games on, goal_game missing / wrong / names a disabled game: unreachable, nothing released
    for (json j : { Both(1), Both(1, "XX"), Both(1, "") }) {
        GameProgress u;
        u.Configure(j);
        CHECK(!u.GoalReached({ "CE", "H2" }) && !u.Decide({ "CE", "H2" }, kMissing).sendGoal && u.Decide({ "CE" }, kMissing).release.empty());
    }
    GameProgress off;
    off.Configure(json{ {"ce_enabled", 1}, {"h2_enabled", 0}, {"final_mission", "The Maw"}, {"goal_mode", 1}, {"goal_game", "H2"} });
    CHECK(off.GoalReached({ "CE" }));  // one game on: that game is the goal game
    // an unknown (newer) goal_mode: no goal, no release, no DLL final
    GameProgress nw;
    nw.Configure(Both(2));
    CHECK(nw.Mode() == GoalMode::Unknown && !nw.GoalReached({ "CE", "H2" }) && nw.Decide({ "CE" }, kMissing).release.empty());
    CHECK(nw.DllMessages({})[1] == "FINAL_MISSION:-1");
    GameProgress wt;   // wrong type = missing = per_game
    wt.Configure(json{ {"ce_enabled", 1}, {"final_mission", "The Maw"}, {"goal_mode", "one_final"} });
    CHECK(wt.Mode() == GoalMode::PerGame && wt.GoalReached({ "CE" }));
    // before the config is known nothing is decided
    GameProgress un;
    CHECK(!un.Decide({ "CE", "H2" }, kMissing).sendGoal && un.Decide({ "CE" }, kMissing).release.empty());
}

int main() {
    TestTables();
    TestOldSeedAndCeOnly();
    TestH2();
    TestEarlyCompletions();
    TestKillTrackerIgnoresH2();
    TestGameOfLocation();
    TestGoalModes();
    TestOneFinal();
    std::printf("game_progress: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
