// Plain C++ tests for pipe_protocol and pending_checks. No framework; exit code != 0 on failure.
#include "pipe_protocol.h"
#include "pending_checks.h"
#include "telemetry_log.h"
#include "shared/ai_event_format.h"
#include "shared/difficulty.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace haloap;

static int g_checks = 0;
static int g_failures = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void TestLocationChecked() {
    CHECK(ParseLocationChecked("LOCATION_CHECKED: 12345") == 12345);
    CHECK(ParseLocationChecked("LOCATION_CHECKED: 12345\r\n") == 12345);
    CHECK(ParseLocationChecked("LOCATION_CHECKED: 9223372036854775807") == INT64_MAX);
    CHECK(!ParseLocationChecked("LOCATION_CHECKED: abc"));
    CHECK(!ParseLocationChecked("LOCATION_CHECKED: "));
    CHECK(!ParseLocationChecked("LOCATION_CHECKED: 12abc"));
    CHECK(!ParseLocationChecked("LOCATION_CHECKED: 12 34"));
    CHECK(!ParseLocationChecked("LOCATION_CHECKED: -5"));
    CHECK(!ParseLocationChecked("LOCATION_CHECKED: 0"));
    CHECK(!ParseLocationChecked("LOCATION_CHECKED: +7"));
    CHECK(!ParseLocationChecked("LOCATION_CHECKED: 9223372036854775808"));
    CHECK(!ParseLocationChecked("LOCATION_CHECKED: 99999999999999999999999999"));
    CHECK(!ParseLocationChecked("LOCATION_CHECKED:12"));
    CHECK(!ParseLocationChecked("OTHER: 12"));
    CHECK(!ParseLocationChecked(""));
}

static void TestMissionComplete() {
    CHECK(ParseMissionComplete("MISSION_COMPLETE: a10") == std::string("a10"));
    CHECK(ParseMissionComplete("MISSION_COMPLETE: d40\n") == std::string("d40"));
    CHECK(!ParseMissionComplete("MISSION_COMPLETE: "));
    CHECK(!ParseMissionComplete("MISSION_COMPLETE:    "));
    CHECK(!ParseMissionComplete("MISSION_COMPLETE: a 10"));
    CHECK(!ParseMissionComplete("MISSION_COMPLETE: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
    CHECK(!ParseMissionComplete(std::string("MISSION_COMPLETE: a\x01") + "b"));
    CHECK(!ParseMissionComplete("MISSION_LOAD: a10"));
    CHECK(!ParseMissionComplete(""));
}

static void TestMissionLoad() {
    CHECK(ParseMissionLoad("MISSION_LOAD: path='levels\\d40\\d40'") == std::string("d40"));
    CHECK(ParseMissionLoad("MISSION_LOAD: levels\\a10\\a10") == std::string("a10"));
    CHECK(!ParseMissionLoad("MISSION_LOAD: path='d40'"));
    CHECK(!ParseMissionLoad("MISSION_LOAD: "));
    CHECK(!ParseMissionLoad("MISSION_LOAD: levels\\"));
    CHECK(!ParseMissionLoad("MISSION_LOAD: levels\\d40"));
    CHECK(!ParseMissionLoad("MISSION_LOAD: levels\\\\x"));
    CHECK(!ParseMissionLoad("CHAPTER:1"));
}

static void TestDifficulty() {
    CHECK(ParseMissionLoad("MISSION_LOAD: path='levels\\a10\\a10' diff=2") == std::string("a10"));  // suffix does not disturb the code
    CHECK(ParseDiffToken("MISSION_LOAD: path='levels\\a10\\a10' diff=2") == 2);
    CHECK(ParseDiffToken("MISSION_LOAD: path='levels\\a10\\a10' diff=0") == 0);
    CHECK(ParseDiffToken("MISSION_LOAD: path='levels\\a10\\a10' diff=-1") == -1);
    CHECK(ParseDiffToken("MISSION_LOAD: path='levels\\a10\\a10'") == -1);            // old DLL: no field
    CHECK(ParseDiffToken("ev=skull diff=3 code=x") == 3);
    CHECK(ParseDiffToken("ev=x diff=- code=x") == -1);
    CHECK(ParseDiffToken("ev=x diff=3x") == -1);
    CHECK(ParseDiffToken("ev=x diff=") == -1);
    CHECK(ParseDiffToken("ev=x diff=1234567") == -1);                                    // too long to be a difficulty
    CHECK(ParseDiffToken("") == -1);
    CHECK(std::string(DifficultyName(0)) == "easy" && std::string(DifficultyName(1)) == "normal");
    CHECK(std::string(DifficultyName(2)) == "heroic" && std::string(DifficultyName(3)) == "legendary");
    CHECK(DifficultyName(4) == nullptr && DifficultyName(-1) == nullptr);
    CHECK(DifficultyJson(3) == "\"legendary\"" && DifficultyJson(9) == "9" && DifficultyJson(-1).empty());
    uint8_t g[0x200] = {};                                                               // fake game globals block
    g[kGlobalsDifficultyOffset] = 2;
    CHECK(DifficultyFromGlobals(g) == 2);
    CHECK(DifficultyFromGlobals(nullptr) == -1);
}

static void TestMissionLoadH2() {
    CHECK(ParseMissionLoad("MISSION_LOAD: path='scenarios\\solo\\03a_oldmombasa\\03a_oldmombasa'") == std::string("03a_oldmombasa"));
    CHECK(ParseMissionLoad("MISSION_LOAD: scenarios\\solo\\08b_deltacontrol\\08b_deltacontrol") == std::string("08b_deltacontrol"));
    CHECK(!ParseMissionLoad("MISSION_LOAD: path='scenarios\\solo\\03a_oldmombasa'"));
    CHECK(!ParseMissionLoad("MISSION_LOAD: path='scenarios\\solo\\\\x'"));
    CHECK(!ParseMissionLoad("MISSION_LOAD: path='scenarios\\multi\\x\\x'"));
}

static void TestChapter() {
    CHECK(ParseChapter("CHAPTER:0") == 0);
    CHECK(ParseChapter("CHAPTER:2") == 2);
    CHECK(ParseChapter("CHAPTER: 3") == 3);
    CHECK(ParseChapter("CHAPTER:999") == 999);
    CHECK(!ParseChapter("CHAPTER:"));
    CHECK(!ParseChapter("CHAPTER:x"));
    CHECK(!ParseChapter("CHAPTER:-1"));
    CHECK(!ParseChapter("CHAPTER:1000"));
    CHECK(!ParseChapter("CHAPTER:99999999999999999999"));
    CHECK(!ParseChapter("CHAPTER:1x"));
    CHECK(!ParseChapter("LOCATION_CHECKED: 1"));
}

static std::string TempPath(const std::string& name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

static void TestPending() {
    // queue while disconnected, confirm removes
    PendingChecks p;
    CHECK(p.Add(10));
    CHECK(!p.Add(10));      // duplicate
    CHECK(!p.Add(0));       // invalid
    CHECK(!p.Add(-4));
    CHECK(p.Add(20));
    CHECK(p.Size() == 2 && p.IsPending(10) && p.IsPending(20));
    CHECK(p.Confirm(10));
    CHECK(!p.IsPending(10) && p.IsConfirmed(10));
    CHECK(!p.Add(10));      // already confirmed by server: never re-queued
    CHECK(!p.Confirm(99));  // not pending
    CHECK(p.IsConfirmed(99) && !p.Add(99));
    CHECK(p.Pending().size() == 1);

    // IDs that aren't locations of the connected slot are dropped (goal mission, a kill milestone above the seed's cap)
    {
        PendingChecks r;
        r.Add(101000); r.Add(110000); r.Add(1010010000);
        CHECK(r.RetainOnly({101000, 102000}) == 2);
        CHECK(r.Size() == 1 && r.IsPending(101000));
        CHECK(r.RetainOnly({101000}) == 0);
    }

    // persistence round-trip (unique name so overlapping test runs can't collide)
    const std::string uniq = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::string path = TempPath("haloap_test_pending_" + uniq + ".txt");
    std::filesystem::remove(path);
    {
        PendingChecks a(path);
        a.Load();
        CHECK(a.Size() == 0);   // missing file
        CHECK(a.Add(111));
        CHECK(a.Add(222));
        CHECK(a.Add(333));
        CHECK(a.Confirm(222));
    }
    CHECK(!std::filesystem::exists(path + ".tmp"));
    {
        PendingChecks b(path);
        b.Load();
        CHECK(b.Size() == 2 && b.IsPending(111) && b.IsPending(333) && !b.IsPending(222));
    }

    // corrupt lines ignored
    {
        std::ofstream f(path, std::ios::trunc);
        f << "101\nabc\n\n-5\n0\n12x\n99999999999999999999\n  202  \n";
    }
    {
        PendingChecks c(path);
        c.Load();
        CHECK(c.Size() == 2 && c.IsPending(101) && c.IsPending(202));
    }
    std::filesystem::remove(path);

    // filename: safe chars, stable, distinguishes server+slot
    std::string n1 = PendingFileName("wss://archipelago.gg:38281", "Player One");
    std::string n2 = PendingFileName("wss://archipelago.gg:38281", "Player Two");
    CHECK(n1 == PendingFileName("wss://archipelago.gg:38281", "Player One"));
    CHECK(n1 != n2);
    CHECK(n1.size() == std::string("haloap_pending_").size() + 16 + 4);
    CHECK(n1.find_first_of("/\\: ") == std::string::npos);
}

// Reconnect reconciliation (Phase 2 QA B1, M1, M2).
static void TestReconcile() {
    const std::string uniq = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const std::string seedA = TempPath("haloap_test_seedA_" + uniq + ".txt");
    const std::string seedB = TempPath("haloap_test_seedB_" + uniq + ".txt");
    const int64_t PoA = 101000, Halo = 102000, Maw = 110000, Kill = 1010010000;

    // Session 1 (seed A): two checks queued before the slot connected; final mission (The Maw) beaten.
    {
        PendingChecks p;            // memory only until connect
        p.Add(PoA); p.Add(Maw);
        p.SetGoalReached("CE");
        p.Bind(seedA);              // slot connects: merged into seed A's file
        CHECK(p.IsPending(PoA) && p.IsPending(Maw) && p.GoalReached("CE"));
        // The Maw is the goal, so its completion is not a location of the slot; PoA is missing (unchecked)
        ReconcileResult r = p.Reconcile({Halo}, {PoA});
        CHECK(r.dropped == 1);                       // Maw dropped
        CHECK(r.toSend.size() == 1 && r.toSend[0] == PoA);
        CHECK(p.GoalReached("CE"));                  // goal kept even though its location was dropped (M1)
        // launcher closes before the server echoes PoA
    }
    // Session 2 (seed A again, after restart): goal and PoA survive
    {
        PendingChecks p;
        p.Bind(seedA);
        CHECK(p.GoalReached("CE") && p.IsPending(PoA));
        ReconcileResult r = p.Reconcile({PoA, Halo}, {Kill});   // server already has PoA now
        CHECK(r.toSend.empty() && p.GoalReached("CE"));
        CHECK(!p.IsPending(PoA));
    }
    // New seed B on the same server address and slot name: inherits nothing (B1)
    {
        PendingChecks p;
        p.Bind(seedB);
        ReconcileResult r = p.Reconcile({}, {PoA, Halo, Kill});
        CHECK(r.toSend.empty() && p.Goals().empty() && p.Size() == 0);
    }
    // Server sent no location list at all: can't judge, keep everything
    {
        PendingChecks p;
        p.Add(Kill);
        ReconcileResult r = p.Reconcile({}, {});
        CHECK(r.dropped == 0 && r.toSend.size() == 1);
    }
    // Different seeds give different files
    CHECK(PendingFileName("seed1", "Player") != PendingFileName("seed2", "Player"));
    // CRLF file and GOAL line load correctly
    {
        std::ofstream f(seedB, std::ios::trunc | std::ios::binary);
        f << "303\r\nGOAL\r\n";
    }
    {
        PendingChecks p(seedB);
        p.Load();
        CHECK(p.IsPending(303) && p.GoalReached("CE") && !p.GoalReached("H2") && p.Size() == 1);  // legacy GOAL = CE
        p.SetGoalReached("H2");   // rewrites the file in the per-game format
        PendingChecks q(seedB);
        q.Load();
        CHECK(q.GoalReached("CE") && q.GoalReached("H2") && q.IsPending(303));
        std::ifstream in(seedB, std::ios::binary);
        std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        all.erase(std::remove(all.begin(), all.end(), '\r'), all.end());  // text-mode write: CRLF on Windows
        CHECK(all == "303\nGOAL:CE\nGOAL:H2\n");
        // flags set before the seed was known survive Bind (union with the file's)
        PendingChecks m;
        m.SetGoalReached("H2");
        m.Bind(seedB);
        CHECK(m.GoalReached("CE") && m.GoalReached("H2") && m.Goals().size() == 2);
    }
    std::filesystem::remove(seedA);
    std::filesystem::remove(seedB);

    // Same run, reconnect to another seed: nothing carries over, the same ID can be queued again, A's file keeps A's
    {
        PendingChecks p;
        p.Bind(seedA);
        p.Add(101000);
        p.Add(102000);
        p.Confirm(102000);
        p.SetGoalReached("CE");
        p.Bind(seedB);
        CHECK(p.Size() == 0);
        CHECK(p.Goals().empty());
        CHECK(p.Add(102000));
        p.Bind(seedB);  // plain reconnect to the same seed keeps everything
        CHECK(p.IsPending(102000));
        p.Bind(seedA);
        CHECK(p.IsPending(101000) && !p.IsPending(102000));
        CHECK(p.GoalReached("CE"));
    }
    std::filesystem::remove(seedA);
    std::filesystem::remove(seedB);
}

static void TestSwarmEvent() {
    const std::string ok = "ah=0xe1a40021 unit=0x00010002 enc=12 sq=3 var=0xe1a40021 ekf=0x20 members=4 tid=8812";
    auto k = ParseSwarmEvent("SWARM: " + ok);
    CHECK(k && k->ah == 0xe1a40021u && k->unit == 0x10002u && k->enc == 12 && k->sq == 3 && k->var == 0xe1a40021u
        && k->ekf == 0x20 && k->members == 4 && k->tid == 8812u);
    auto u = ParseSwarmEvent("SWARM: unit=0xffffffff ah=0xffffffff enc=-1 sq=-1 var=0xffffffff ekf=0xff members=-1 tid=0");
    CHECK(u && u->enc == -1 && u->sq == -1 && u->members == -1 && u->ekf == 0xff && u->tid == 0);
    CHECK(!ParseSwarmEvent(""));
    CHECK(!ParseSwarmEvent("SWARM:" + ok));
    CHECK(!ParseSwarmEvent("KILL: " + ok));
    CHECK(!ParseSwarmEvent("SWARM: " + ok + " extra=1"));
    CHECK(!ParseSwarmEvent("SWARM: " + ok + " tid=2"));            // duplicate key
    CHECK(!ParseSwarmEvent("SWARM: ah=0x1  unit=0x2 enc=1 sq=1 var=0x1 ekf=0x0 members=1 tid=1"));  // double space
    CHECK(!ParseSwarmEvent("SWARM: ah=0x1 unit=0x2 enc=65536 sq=1 var=0x1 ekf=0x0 members=1 tid=1"));
    CHECK(!ParseSwarmEvent("SWARM: ah=0x1 unit=0x2 enc=1 sq=1 var=0x1 ekf=0x100 members=1 tid=1"));
    CHECK(!ParseSwarmEvent("SWARM: ah=0x1 unit=0x2 enc=1 sq=1 var=0x1 ekf=0x0 members=32768 tid=1"));
    CHECK(!ParseSwarmEvent("SWARM: ah=0x1 unit=0x2 enc=1 sq=1 var=0x1 ekf=0x0 members=-2 tid=1"));
    CHECK(!ParseSwarmEvent("SWARM: ah=0x100000000 unit=0x2 enc=1 sq=1 var=0x1 ekf=0x0 members=1 tid=1"));
    for (const char* key : { "ah", "unit", "enc", "sq", "var", "ekf", "members", "tid" }) {
        std::string msg = "SWARM:";
        size_t pos = 0;
        while (pos < ok.size()) {
            size_t sp = ok.find(' ', pos);
            std::string tok = ok.substr(pos, sp == std::string::npos ? std::string::npos : sp - pos);
            if (tok.rfind(std::string(key) + "=", 0) != 0) msg += " " + tok;
            if (sp == std::string::npos) break;
            pos = sp + 1;
        }
        CHECK(!ParseSwarmEvent(msg));
    }
    // A SWARM line can never become a KILL/ERASE event (so it cannot reach the KillTracker), and vice versa.
    CHECK(!ParseAiEvent("SWARM: " + ok));
    CHECK(!ParseSwarmEvent("KILL: ah=0x1 unit=0x2 enc=1 sq=2 pl=3 var=0x10 fl=0x01 ekf=0x00 living=1 total=2 el=1 et=2 tid=5"));
    // The DLL's snprintf format round-trips through the parser.
    char buf[256];
    snprintf(buf, sizeof(buf), haloap::kSwarmEventFormat, 0xe1a40021u, 0x10002u, 12, 3, 0xe1a40021u, 0x20u, 4, 8812ul);
    auto rt = ParseSwarmEvent(buf);
    CHECK(rt && rt->members == 4 && rt->enc == 12 && rt->ekf == 0x20 && rt->tid == 8812u);
}

static void TestAiEvent() {
    auto k = ParseAiEvent("KILL: ah=0xe1a40021 unit=0xffffffff enc=12 sq=3 pl=-1 var=0xe1a40021 fl=0x00 ekf=0x20 "
        "living=0 total=4 el=0 et=9 tid=8812");
    CHECK(k && k->kill && k->ah == 0xe1a40021u && k->unit == 0xffffffffu && k->enc == 12 && k->sq == 3 && k->pl == -1
        && k->var == 0xe1a40021u && k->flags == 0 && k->ekf == 0x20 && k->living == 0 && k->total == 4
        && k->el == 0 && k->et == 9 && k->tid == 8812u);
    auto e = ParseAiEvent("ERASE: ah=0x1 unit=0x2 enc=-1 sq=-1 pl=65535 var=0x00000001 fl=0xff ekf=0x60 living=-1 "
        "total=32767 el=-1 et=-1 tid=0");
    CHECK(e && !e->kill && e->enc == -1 && e->pl == 65535 && e->flags == 0xff && e->ekf == 0x60 && e->living == -1
        && e->total == 32767 && e->el == -1 && e->et == -1 && e->tid == 0);
    const std::string ok = "ah=0x1 unit=0x2 enc=1 sq=2 pl=3 var=0x10 fl=0x01 ekf=0x00 living=1 total=2 el=1 et=2 tid=5";
    CHECK(ParseAiEvent("KILL: " + ok));
    CHECK(!ParseAiEvent(""));
    CHECK(!ParseAiEvent("DEATH: " + ok));
    CHECK(!ParseAiEvent("KILL:" + ok));
    // drop one key at a time -> every key is required
    for (const char* key : { "ah", "unit", "enc", "sq", "pl", "var", "fl", "ekf", "living", "total", "el", "et", "tid" }) {
        std::string msg = "KILL:";
        size_t pos = 0;
        while (pos < ok.size()) {
            size_t sp = ok.find(' ', pos);
            std::string tok = ok.substr(pos, sp == std::string::npos ? std::string::npos : sp - pos);
            if (tok.rfind(std::string(key) + "=", 0) != 0) msg += " " + tok;
            if (sp == std::string::npos) break;
            pos = sp + 1;
        }
        CHECK(!ParseAiEvent(msg));
    }
    CHECK(!ParseAiEvent("KILL: " + ok + " tid=6"));                                             // duplicate
    CHECK(!ParseAiEvent("KILL: " + ok + " extra=1"));                                           // extra key
    CHECK(!ParseAiEvent("KILL: ah=0x1 unit=0x2 enc=x sq=2 pl=3 var=0x10 fl=0x01 ekf=0 living=1 total=2 el=1 et=2 tid=5"));
    CHECK(!ParseAiEvent("KILL: ah=0x1 unit=0x2 enc=1 sq=2 pl=3 var=0xzz fl=0x01 ekf=0 living=1 total=2 el=1 et=2 tid=5"));
    CHECK(!ParseAiEvent("KILL: ah=-1 unit=0x2 enc=1 sq=2 pl=3 var=0x10 fl=0x01 ekf=0 living=1 total=2 el=1 et=2 tid=5"));
    CHECK(!ParseAiEvent("KILL: ah=0x1ffffffff unit=0x2 enc=1 sq=2 pl=3 var=0x10 fl=0x01 ekf=0 living=1 total=2 el=1 et=2 tid=5"));
    CHECK(!ParseAiEvent("KILL: ah=0x1 unit=0x2 enc=-2 sq=2 pl=3 var=0x10 fl=0x01 ekf=0 living=1 total=2 el=1 et=2 tid=5"));
    CHECK(!ParseAiEvent("KILL: ah=0x1 unit=0x2 enc=65536 sq=2 pl=3 var=0x10 fl=0x01 ekf=0 living=1 total=2 el=1 et=2 tid=5"));
    CHECK(!ParseAiEvent("KILL: ah=0x1 unit=0x2 enc=1 sq=2 pl=3 var=0x10 fl=0x100 ekf=0 living=1 total=2 el=1 et=2 tid=5"));
    CHECK(!ParseAiEvent("KILL: ah=0x1 unit=0x2 enc=1 sq=2 pl=3 var=0x10 fl=0x01 ekf=0x100 living=1 total=2 el=1 et=2 tid=5"));
    CHECK(!ParseAiEvent("KILL: ah=0x1 unit=0x2 enc=1 sq=2 pl=3 var=0x10 fl=0x01 ekf=0 living=32768 total=2 el=1 et=2 tid=5"));
    CHECK(!ParseAiEvent("KILL: ah=0x1 unit=0x2 enc=1 sq=2 pl=3 var=0x10 fl=0x01 ekf=0 living=1 total=2 el=-2 et=2 tid=5"));
    CHECK(!ParseAiEvent("KILL: ah=0x1 unit=0x2 enc=1 sq=2 pl=3 var=0x10 fl=0x01 ekf=0 living=1 total=2 el=1 et=32768 tid=5"));
    CHECK(!ParseAiEvent("KILL: ah=0x1 unit=0x2 enc=1 sq=2 pl=3 var=0x10 fl=0x01 ekf=0 living=1 total=2 el=1 et=2 tid=-1"));
    CHECK(!ParseAiEvent("KILL: ah=0x1  unit=0x2 enc=1 sq=2 pl=3 var=0x10 fl=0x01 ekf=0 living=1 total=2 el=1 et=2 tid=5"));
    CHECK(!ParseAiEvent("KILL: ah=0x1 unit=0x2 enc=99999999999999999999 sq=2 pl=3 var=0x10 fl=0x01 ekf=0 living=1 total=2 el=1 et=2 tid=5"));

    // m5: the DLL's snprintf format must round-trip through the parser.
    char buf[256];
    std::snprintf(buf, sizeof buf, haloap::kAiEventFormat, "KILL", 0xe1a40021u, 0xffffffffu, 4, 2, -1, 0x1234abcdu,
        0x01u, 0x20u, 0, 3, 0, 7, 99ul);
    auto rt = ParseAiEvent(buf);
    CHECK(rt && rt->kill && rt->ah == 0xe1a40021u && rt->unit == 0xffffffffu && rt->enc == 4 && rt->sq == 2
        && rt->pl == -1 && rt->var == 0x1234abcdu && rt->flags == 1 && rt->ekf == 0x20 && rt->living == 0
        && rt->total == 3 && rt->el == 0 && rt->et == 7 && rt->tid == 99u);
    std::snprintf(buf, sizeof buf, haloap::kAiEventFormat, "ERASE", 0u, 0u, -1, -1, -1, 0u, 0u, 0u, -1, -1, -1, -1, 0ul);
    auto rt2 = ParseAiEvent(buf);
    CHECK(rt2 && !rt2->kill && rt2->enc == -1 && rt2->el == -1);
}

static std::string ReadAll(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Replaces the digits after "\"t\":" with N so output is comparable exactly.
static std::string NormT(const std::string& s) {
    std::string out;
    size_t i = 0;
    const std::string key = "\"t\":";
    while (i < s.size()) {
        if (s.compare(i, key.size(), key) == 0) {
            out += key + "N";
            i += key.size();
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
        } else out += s[i++];
    }
    return out;
}

static void TestTelemetry() {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / ("haloap_tlm_" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
    std::string d = dir.string() + "/";
    {
        TelemetryLog log(d, "build1", "0.1");
        CHECK(log.Path().empty());  // lazy
        CHECK(fs::is_empty(dir));
        log.Rejected();  // before the first mission_load: reset by MissionLoad
        log.MissionLoad("a10");
        log.Pipe(true);
        auto k = ParseAiEvent("KILL: ah=0xe1a40021 unit=0x00010002 enc=12 sq=3 pl=-1 var=0xe1a40021 fl=0x01 ekf=0x20 "
            "living=0 total=4 el=0 et=9 tid=8812");
        auto e = ParseAiEvent("ERASE: ah=0x5 unit=0xffffffff enc=-1 sq=-1 pl=7 var=0x00000005 fl=0x00 ekf=0x00 "
            "living=-1 total=-1 el=-1 et=-1 tid=1");
        CHECK(k && e);
        log.Death("a10", *k);
        log.Erase("we\"ird\\", *e);
        auto sw = ParseSwarmEvent("SWARM: ah=0x00000007 unit=0x00010003 enc=5 sq=2 var=0xe1a40022 ekf=0x00 members=3 tid=9");
        CHECK(sw);
        log.Swarm("a10", *sw);
        log.Rejected();
        log.Rejected();
        log.Pipe(false);
        log.MissionEnd("a10", "won");
        log.MissionLoad("a30");
        log.H2("mission_load code=03a\t\"x\"\x01 \xc3\xa9 bad=\xff.");
        log.H2(std::string(5000, 'z'));
        log.H2("ev=skull n=4 tid=5 tick=900 path=notify_event n=5 plr=0 src=73de5e code=03a_oldmombasa diff=3");  // 8.H2.9b skull line
        log.MissionLoad("a50", 3);   // PLAN 5.5: legendary
        log.MissionLoad("b30", 7);   // unknown value: the raw number
        log.MissionLoad("b40", -1);  // unreadable: no field
        log.MissionLoad("c10", 2);
        log.H2("ev=start n=1 mode=1 map=2 diff=-");  // unreadable on a Halo 2 line: no field
        log.H3("ev=start n=2 tid=9 kind=load ok=1 mode=1 camp=1 map=3010 diff=2 path=010_jungle");  // PLAN 8.H3.2
        std::string got = NormT(ReadAll(log.Path()));
        std::string want =
            "{\"ev\":\"session\",\"t\":N,\"gen\":0,\"build\":\"build1\",\"launcher\":\"0.1\"}\n"
            "{\"ev\":\"mission_load\",\"t\":N,\"gen\":1,\"mission\":\"a10\"}\n"
            "{\"ev\":\"pipe\",\"t\":N,\"gen\":1,\"state\":\"connected\"}\n"
            "{\"ev\":\"death\",\"t\":N,\"gen\":1,\"mission\":\"a10\",\"enc\":12,\"sq\":3,\"pl\":-1,\"variant\":33,\"swarm\":true,"
            "\"var_handle\":\"0xe1a40021\",\"ah\":\"0xe1a40021\",\"unit\":\"0x00010002\",\"ekf\":\"0x20\",\"el\":0,\"et\":9,"
            "\"tid\":8812}\n"
            "{\"ev\":\"squad\",\"t\":N,\"gen\":1,\"mission\":\"a10\",\"enc\":12,\"sq\":3,\"living\":0,\"spawned\":4}\n"
            "{\"ev\":\"erase\",\"t\":N,\"gen\":1,\"mission\":\"we\\\"ird\\\\\",\"enc\":-1,\"sq\":-1,\"pl\":7,\"variant\":5,\"swarm\":false,"
            "\"var_handle\":\"0x00000005\",\"ah\":\"0x00000005\",\"unit\":\"0xffffffff\",\"ekf\":\"0x00\",\"el\":-1,\"et\":-1,"
            "\"tid\":1}\n"
            "{\"ev\":\"squad\",\"t\":N,\"gen\":1,\"mission\":\"we\\\"ird\\\\\",\"enc\":-1,\"sq\":-1,\"living\":-1,\"spawned\":-1}\n"
            "{\"ev\":\"swarm\",\"t\":N,\"gen\":1,\"mission\":\"a10\",\"enc\":5,\"sq\":2,\"variant\":34,"
            "\"var_handle\":\"0xe1a40022\",\"ah\":\"0x00000007\",\"unit\":\"0x00010003\",\"ekf\":\"0x00\",\"members\":3,"
            "\"tid\":9}\n"
            "{\"ev\":\"pipe\",\"t\":N,\"gen\":1,\"state\":\"disconnected\"}\n"
            "{\"ev\":\"mission_end\",\"t\":N,\"gen\":1,\"mission\":\"a10\",\"reason\":\"won\",\"rejected\":2}\n"
            "{\"ev\":\"mission_load\",\"t\":N,\"gen\":2,\"mission\":\"a30\"}\n"
            "{\"ev\":\"h2\",\"t\":N,\"gen\":2,\"msg\":\"mission_load code=03a\\t\\\"x\\\"\\u0001 \xc3\xa9 bad=\xef\xbf\xbd.\"}\n"
            "{\"ev\":\"h2\",\"t\":N,\"gen\":2,\"msg\":\"" + std::string(2048, 'z') + "\"}\n"
            "{\"ev\":\"h2\",\"t\":N,\"gen\":2,\"msg\":\"ev=skull n=4 tid=5 tick=900 path=notify_event n=5 plr=0 src=73de5e "
            "code=03a_oldmombasa diff=3\",\"difficulty\":\"legendary\"}\n"
            "{\"ev\":\"mission_load\",\"t\":N,\"gen\":3,\"mission\":\"a50\",\"difficulty\":\"legendary\"}\n"
            "{\"ev\":\"mission_load\",\"t\":N,\"gen\":4,\"mission\":\"b30\",\"difficulty\":7}\n"
            "{\"ev\":\"mission_load\",\"t\":N,\"gen\":5,\"mission\":\"b40\"}\n"
            "{\"ev\":\"mission_load\",\"t\":N,\"gen\":6,\"mission\":\"c10\",\"difficulty\":\"heroic\"}\n"
            "{\"ev\":\"h2\",\"t\":N,\"gen\":6,\"msg\":\"ev=start n=1 mode=1 map=2 diff=-\"}\n"
            "{\"ev\":\"h3\",\"t\":N,\"gen\":6,\"msg\":\"ev=start n=2 tid=9 kind=load ok=1 mode=1 camp=1 map=3010 diff=2 path=010_jungle\","
            "\"difficulty\":\"heroic\"}\n";
        CHECK(got == want);
        if (got != want) std::printf("got:\n%s\n", got.c_str());
    }
    CHECK(JsonString(std::string("a\tb\x01")) == "\"a\\tb\\u0001\"");
    {   // unopenable directory: no throw, no file
        TelemetryLog bad(d + "no_such_dir/");
        bad.MissionLoad("a10");
        CHECK(bad.Path().empty());
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

int main() {
    TestAiEvent();
    TestSwarmEvent();
    TestTelemetry();
    TestReconcile();
    TestLocationChecked();
    TestMissionComplete();
    TestMissionLoad();
    TestMissionLoadH2();
    TestDifficulty();
    TestChapter();
    TestPending();
    std::printf("native-tests: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
