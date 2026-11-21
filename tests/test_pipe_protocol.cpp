// Plain C++ tests for pipe_protocol and pending_checks. No framework; exit code != 0 on failure.
#include "pipe_protocol.h"
#include "pending_checks.h"

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

    // IDs that aren't locations of the connected slot are dropped (goal mission, killsanity off)
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

int main() {
    TestReconcile();
    TestLocationChecked();
    TestMissionComplete();
    TestMissionLoad();
    TestChapter();
    TestPending();
    std::printf("native-tests: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
