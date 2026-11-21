// Plain C++ tests for pipe_protocol and pending_checks. No framework; exit code != 0 on failure.
#include "pipe_protocol.h"
#include "pending_checks.h"
#include "telemetry_log.h"
#include "shared/ai_event_format.h"

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
        p.SetGoalReached();
        p.Bind(seedA);              // slot connects: merged into seed A's file
        CHECK(p.IsPending(PoA) && p.IsPending(Maw) && p.GoalReached());
        // The Maw is the goal, so its completion is not a location of the slot; PoA is missing (unchecked)
        ReconcileResult r = p.Reconcile({Halo}, {PoA});
        CHECK(r.dropped == 1);                       // Maw dropped
        CHECK(r.toSend.size() == 1 && r.toSend[0] == PoA);
        CHECK(r.sendGoal);                           // goal kept even though its location was dropped (M1)
        // launcher closes before the server echoes PoA
    }
    // Session 2 (seed A again, after restart): goal and PoA survive
    {
        PendingChecks p;
        p.Bind(seedA);
        CHECK(p.GoalReached() && p.IsPending(PoA));
        ReconcileResult r = p.Reconcile({PoA, Halo}, {Kill});   // server already has PoA now
        CHECK(r.toSend.empty() && r.sendGoal);
        CHECK(!p.IsPending(PoA));
    }
    // New seed B on the same server address and slot name: inherits nothing (B1)
    {
        PendingChecks p;
        p.Bind(seedB);
        ReconcileResult r = p.Reconcile({}, {PoA, Halo, Kill});
        CHECK(r.toSend.empty() && !r.sendGoal && p.Size() == 0);
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
        CHECK(p.IsPending(303) && p.GoalReached() && p.Size() == 1);
    }
    std::filesystem::remove(seedA);
    std::filesystem::remove(seedB);
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
        log.Rejected();
        log.Rejected();
        log.Pipe(false);
        log.MissionEnd("a10", "won");
        log.MissionLoad("a30");
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
            "{\"ev\":\"pipe\",\"t\":N,\"gen\":1,\"state\":\"disconnected\"}\n"
            "{\"ev\":\"mission_end\",\"t\":N,\"gen\":1,\"mission\":\"a10\",\"reason\":\"won\",\"rejected\":2}\n"
            "{\"ev\":\"mission_load\",\"t\":N,\"gen\":2,\"mission\":\"a30\"}\n";
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
    TestTelemetry();
    TestReconcile();
    TestLocationChecked();
    TestMissionComplete();
    TestMissionLoad();
    TestChapter();
    TestPending();
    std::printf("native-tests: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
