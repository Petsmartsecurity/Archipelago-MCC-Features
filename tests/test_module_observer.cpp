// Plain C++ tests for the shared observer plumbing: hooks/module_observer_logic.h (line builder, lifecycle decisions, call scan),
// shared/observe_setting.h (the observe_logging switch: config file + pipe line) and the vendored MinHook addition
// MH_ForgetHook (QA 8.H3.2 m5: stale entries of an unmapped DLL). No framework; exit code != 0 on failure.
#define NOMINMAX
#include "hooks/module_observer_logic.h"
#include "shared/observe_setting.h"
#include "minhook/MinHook.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>

using namespace haloap;

static int g_checks = 0;
static int g_failures = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void TestMsg() {
    // The line format the summarisers and docs/telemetry-format.md depend on: byte-identical to the pre-merge builders.
    obs::Msg a("H2", "game_start", 7, 1234);
    a.Add("kind=%s ok=%d", "load", 1);
    a.Add("path=%s", "01\x01_jungle\xc3\xa9");
    CHECK(std::strcmp(a.Finish(), "H2: ev=game_start n=7 tid=1234 kind=load ok=1 path=01__jungle__") == 0);
    obs::Msg b("H3", "start", 1, 2);
    CHECK(std::strcmp(b.Finish(), "H3: ev=start n=1 tid=2") == 0);
    obs::Msg big("H3", "x", 1, 1);
    for (int i = 0; i < 100; i++) big.Add("k%d=%s", i, "vvvvvvvvvv");
    CHECK(big.truncated());
    CHECK(big.size() < obs::kMsgMax);
    CHECK(std::strncmp(big.Finish(), "H3: ev=x n=1 tid=1", 18) == 0);
}

static void TestReattach() {
    using obs::Reattach;
    CHECK(obs::DecideReattach(true, 6, 6) == Reattach::Full);   // identity changed
    CHECK(obs::DecideReattach(false, 0, 0) == Reattach::None);  // nothing installed
    CHECK(obs::DecideReattach(false, 6, 0) == Reattach::Full);  // reload: no prologue is ours
    CHECK(obs::DecideReattach(false, 6, 5) == Reattach::DropChanged);
    CHECK(obs::DecideReattach(false, 6, 6) == Reattach::None);
}

static void TestPresence() {
    using obs::Presence;
    CHECK(obs::ClassifyPresence(0, 0) == Presence::Absent);
    CHECK(obs::ClassifyPresence(0x1000, 0x1000) == Presence::Same);
    CHECK(obs::ClassifyPresence(0, 0x1000) == Presence::Attach);
    CHECK(obs::ClassifyPresence(0x1000, 0x2000) == Presence::Replaced);
    CHECK(obs::ClassifyPresence(0x1000, 0) == Presence::Unloaded);
}

static void TestSchedule() {
    CHECK(!obs::RetryDue(0, 1999) && obs::RetryDue(0, 2000));
    CHECK(!obs::RetryDue(1, 9999) && obs::RetryDue(1, 10000));
    CHECK(!obs::RetryDue(2, 1000000) && !obs::RetryDue(-1, 1000000));  // only two retries
    int c = 0, due = 0;
    for (int i = 0; i < 40; i++) due += obs::IntegrityDue(&c) ? 1 : 0;
    CHECK(due == 2 && c == 0);  // every 20th slice
    CHECK(!obs::ShellRepeatDue(0) && !obs::ShellRepeatDue(63) && obs::ShellRepeatDue(64) && obs::ShellRepeatDue(128));
}

static void TestAppendName() {
    char list[16] = {};
    obs::AppendName(list, sizeof(list), "a");
    obs::AppendName(list, sizeof(list), "bb");
    CHECK(std::strcmp(list, "a,bb") == 0);
    obs::AppendName(list, sizeof(list), "cccccccccccccccc");  // does not fit: still terminated, no overflow
    CHECK(std::strlen(list) < sizeof(list));
}

static void TestCallScan() {
    // 0x00: E8 rel32 -> 0x20 ; 0x0A: E8 rel32 -> 0x20 ; 0x14: E8 rel32 -> 0x40 (other target)
    uint8_t code[0x60] = {};
    auto call = [&](size_t at, size_t to) {
        code[at] = 0xE8;
        int32_t rel = static_cast<int32_t>(to) - static_cast<int32_t>(at + 5);
        std::memcpy(code + at + 1, &rel, 4);
    };
    call(0x00, 0x20); call(0x0A, 0x20); call(0x14, 0x40);
    CHECK(obs::CallTargets(code, code + 0x20));
    CHECK(!obs::CallTargets(code, code + 0x40));
    CHECK(!obs::CallTargets(code + 1, code + 0x20));  // not an E8
    const uint8_t* sites[4] = {};
    CHECK(obs::FindCallSites(code, sizeof(code), code + 0x20, sites, 4) == 2);
    CHECK(sites[0] == code && sites[1] == code + 0x0A);
    CHECK(obs::FindCallSites(code, sizeof(code), code + 0x20, sites, 1) == 2 && sites[0] == code);  // count may exceed cap
    CHECK(obs::FindCallSites(code, 4, code + 0x20, sites, 4) == 0);                                    // too short for a call
}

static void TestObserveSetting() {
    bool on = false;
    CHECK(ParseObserveMessage("OBSERVE: 1", &on) && on);
    CHECK(ParseObserveMessage("OBSERVE: 0", &on) && !on);
    on = true;
    CHECK(!ParseObserveMessage("OBSERVE: 2", &on) && on);   // rejected, untouched
    CHECK(!ParseObserveMessage("OBSERVE: ", &on));
    CHECK(!ParseObserveMessage("OBSERVE: 01", &on));
    CHECK(!ParseObserveMessage("OBSERVE:0", &on));
    CHECK(!ParseObserveMessage("SKULLSANITY: 1", &on));
    CHECK(!ParseObserveMessage(nullptr, &on));
    CHECK(ObserveMessage(true) == "OBSERVE: 1" && ObserveMessage(false) == "OBSERVE: 0");
    bool rt = false;
    CHECK(ParseObserveMessage(ObserveMessage(false).c_str(), &rt) && !rt);

    // config file: line 1 = game dir; default on
    CHECK(ParseObserveLogging(""));
    CHECK(ParseObserveLogging("D:\\Games\\MCC"));
    CHECK(ParseObserveLogging("D:\\Games\\MCC\n"));
    CHECK(!ParseObserveLogging("D:\\Games\\MCC\nobserve_logging=0\n"));
    CHECK(!ParseObserveLogging("D:\\Games\\MCC\r\nobserve_logging=0\r\n"));  // Notepad CRLF
    CHECK(!ParseObserveLogging("D:\\Games\\MCC\n  observe_logging = off  "));
    CHECK(ParseObserveLogging("D:\\Games\\MCC\nOBSERVE_LOGGING=0"));  // key is case-sensitive: unknown key, stays on
    CHECK(ParseObserveLogging("D:\\Games\\MCC\nobserve_logging=1"));
    CHECK(ParseObserveLogging("D:\\Games\\MCC\nobserve_logging=maybe"));          // typo keeps the default
    CHECK(ParseObserveLogging("D:\\Games\\MCC\nobserve_logging="));
    CHECK(!ParseObserveLogging("D:\\Games\\MCC\nobserve_logging=1\nobserve_logging=0"));  // last wins
    CHECK(ParseObserveLogging("D:\\Games\\MCC\nobserve_logging=0\nobserve_logging=1"));
    CHECK(ParseObserveLogging("observe_logging=0"));  // line 1 is the game directory, never a setting
    CHECK(ParseObserveLogging("D:\\Games\\MCC\nsomething_else=0"));
    CHECK(!ParseBoolWord(" No ", true) && ParseBoolWord("TRUE", false) && !ParseBoolWord("False", true));

    // game dir load / save keep the settings
    CHECK(ConfigGameDir("D:\\Games\\MCC\r\nobserve_logging=0\r\n") == "D:\\Games\\MCC");
    CHECK(ConfigGameDir("D:\\Games\\MCC") == "D:\\Games\\MCC");
    CHECK(ConfigGameDir("").empty());
    CHECK(ReplaceConfigGameDir("old\nobserve_logging=0\n", "new") == "new\nobserve_logging=0\n");
    CHECK(ReplaceConfigGameDir("old\r\nobserve_logging=0", "") == "\nobserve_logging=0");  // path cleared, setting kept
    CHECK(ReplaceConfigGameDir("old", "new") == "new");
    CHECK(ReplaceConfigGameDir("", "new") == "new");
}

// ---- MH_ForgetHook (QA 8.H3.2 m5) against a real unmapped "module" ------------------------------------------------------
static int __cdecl Detour() { return 2; }

static uint8_t* MapFakeModule(void* where) {
    auto p = static_cast<uint8_t*>(VirtualAlloc(where, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!p) return nullptr;
    const uint8_t fn[] = { 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC };  // mov eax,1 ; ret
    std::memcpy(p + 0x20, fn, sizeof(fn));
    return p;
}

static void TestForgetHook() {
    CHECK(MH_Initialize() == MH_OK);
    uint8_t* page = MapFakeModule(nullptr);
    CHECK(page != nullptr);
    if (!page) return;
    void* target = page + 0x20;
    void* orig = nullptr;
    CHECK(MH_CreateHook(target, reinterpret_cast<void*>(Detour), &orig) == MH_OK);
    CHECK(MH_EnableHook(target) == MH_OK);
    CHECK(static_cast<uint8_t*>(target)[0] == 0xE9);
    CHECK(reinterpret_cast<int (*)()>(target)() == 2);  // the detour runs

    // The DLL is unmapped (FreeLibrary by the game). MinHook can no longer restore it: the entry stays "enabled".
    CHECK(VirtualFree(page, 0, MEM_RELEASE) != 0);
    CHECK(MH_RemoveHook(target) == MH_ERROR_MEMORY_PROTECT);

    // Forgetting drops the entry without writing to the dead address; a second call finds nothing.
    CHECK(MH_ForgetHook(target) == MH_OK);
    CHECK(MH_ForgetHook(target) == MH_ERROR_NOT_CREATED);

    // Something else is mapped at the same address later (here: the same page, other bytes). A hook created there works,
    // and shutting MinHook down no longer stops at the stale entry or writes the old backup bytes into the new code.
    uint8_t* again = MapFakeModule(page);
    CHECK(again == page);
    if (again) {
        again[0x20] = 0xB8; again[0x21] = 0x07;  // different first bytes than the dead image had
        CHECK(MH_CreateHook(again + 0x20, reinterpret_cast<void*>(Detour), &orig) == MH_OK);
        CHECK(MH_RemoveHook(again + 0x20) == MH_OK);
        CHECK(again[0x20] == 0xB8 && again[0x21] == 0x07);  // restored exactly what was there
    }
    // A live hook forgotten after its image went away must not make MH_Uninitialize fail.
    uint8_t* live = again ? again : MapFakeModule(nullptr);
    if (live) {
        CHECK(MH_CreateHook(live + 0x20, reinterpret_cast<void*>(Detour), &orig) == MH_OK);
        CHECK(MH_EnableHook(live + 0x20) == MH_OK);
        VirtualFree(live, 0, MEM_RELEASE);
        CHECK(MH_ForgetHook(live + 0x20) == MH_OK);
    }
    CHECK(MH_Uninitialize() == MH_OK);
}

int main() {
    TestMsg();
    TestReattach();
    TestPresence();
    TestSchedule();
    TestAppendName();
    TestCallScan();
    TestObserveSetting();
    TestForgetHook();
    std::printf("test_module_observer: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
