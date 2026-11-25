// Plain C++ tests for the pure logic of the Halo 3 observe hooks (hooks/h3_observe_logic.h). No framework; exit code != 0 on failure.
#define NOMINMAX
#include "hooks/h3_observe_logic.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace haloap;

static int g_checks = 0;
static int g_failures = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void TestMsg() {
    h3::Msg m("start", 7, 1234);
    m.Add("kind=%s ok=%d", "load", 1);
    m.Add("path=%s", "01\x01_jungle\xc3\xa9");
    CHECK(std::strcmp(m.Finish(), "H3: ev=start n=7 tid=1234 kind=load ok=1 path=01__jungle__") == 0);
    h3::Msg big("x", 1, 1);
    for (int i = 0; i < 100; i++) big.Add("k%d=%s", i, "vvvvvvvvvv");
    CHECK(big.truncated());
    CHECK(big.size() < h3::kMsgMax);
    CHECK(std::strncmp(big.Finish(), "H3: ev=x n=1 tid=1", 18) == 0);
}

static void TestPathTail() {
    char out[40];
    CHECK(h3::SanitizePathTail("levels\\solo\\010_jungle\\010_jungle", 0x104, out, sizeof(out)) == 10 && std::strcmp(out, "010_jungle") == 0);
    CHECK(h3::SanitizePathTail("a/b/c d", 0x104, out, sizeof(out)) == 3 && std::strcmp(out, "c_d") == 0);
    CHECK(h3::SanitizePathTail("noseparator", 0x104, out, sizeof(out)) == 11);
    CHECK(h3::SanitizePathTail("x\\", 0x104, out, sizeof(out)) == 0 && out[0] == 0);
    CHECK(h3::SanitizePathTail(nullptr, 5, out, sizeof(out)) == 0);
    char small[4];
    CHECK(h3::SanitizePathTail("dir\\abcdefgh", 0x104, small, sizeof(small)) == 3 && std::strcmp(small, "abc") == 0);  // cap respected
    const char unterminated[4] = { 'a', 'b', 'c', 'd' };
    CHECK(h3::SanitizePathTail(unterminated, 4, out, sizeof(out)) == 4);  // maxChars bounds the read
}

static void TestClassify() {
    // start: load / reset / other, unverified link (0) never matches
    CHECK(h3::ClassifyStart(0x100, 0x100, 0x200) == h3::StartKind::Load);
    CHECK(h3::ClassifyStart(0x200, 0x100, 0x200) == h3::StartKind::Reset);
    CHECK(h3::ClassifyStart(0x300, 0x100, 0x200) == h3::StartKind::Other);
    CHECK(h3::ClassifyStart(0, 0, 0) == h3::StartKind::Other);
    // change_immediate callers (QA 8.H3.1 m3)
    CHECK(h3::ClassifyChangeSrc(1, 1, 2, 3) == h3::ChangeSrc::Queue);
    CHECK(h3::ClassifyChangeSrc(2, 1, 2, 3) == h3::ChangeSrc::Checkpoint);
    CHECK(h3::ClassifyChangeSrc(3, 1, 2, 3) == h3::ChangeSrc::Menu);
    CHECK(h3::ClassifyChangeSrc(4, 1, 2, 3) == h3::ChangeSrc::Other);
    CHECK(h3::ClassifyChangeSrc(0, 0, 0, 0) == h3::ChangeSrc::Other);
    CHECK(std::strcmp(h3::ChangeSrcName(h3::ChangeSrc::Checkpoint), "checkpoint") == 0);
    CHECK(h3::IsQuit(true, 1) && h3::IsQuit(false, 3) && !h3::IsQuit(false, 1) && !h3::IsQuit(false, 2));
    // deaths: only a verified link is claimed
    CHECK(h3::ClassifyDeath(10, 10, 20) == h3::DeathKind::Kill);
    CHECK(h3::ClassifyDeath(20, 10, 20) == h3::DeathKind::ScriptKill);
    CHECK(h3::ClassifyDeath(30, 10, 20) == h3::DeathKind::Unknown);
    CHECK(h3::ClassifyDeath(10, 0, 0) == h3::DeathKind::Unknown);
    CHECK(h3::ClassifyDeath(0, 0, 0) == h3::DeathKind::Unknown);
    CHECK(std::strcmp(h3::DeathKindName(h3::DeathKind::ScriptKill), "script") == 0);
}

static void TestWonAndErase() {
    CHECK(h3::DecideWon(0, 0, 4).first && h3::DecideWon(0, 9, 4).log);          // was == 0: always the first, always logged
    CHECK(!h3::DecideWon(1, 0, 4).first && h3::DecideWon(1, 3, 4).log);          // repeats logged up to the cap
    CHECK(!h3::DecideWon(1, 4, 4).log);
    CHECK(!h3::DecideWon(-1, 0, 4).first);                                       // unreadable is never "first"
    CHECK(h3::EraseLogDue(1) && h3::EraseLogDue(h3::kEraseLogFirst));
    CHECK(!h3::EraseLogDue(h3::kEraseLogFirst + 1));
    CHECK(h3::EraseLogDue(h3::kEraseLogFirst + 50 - (h3::kEraseLogFirst % 50)));  // next multiple of 50 above 400
}

static void TestDataElement() {
    const uint32_t stride = 0xAA0;
    std::vector<uint8_t> data(stride * 4, 0);
    uint16_t salt = 0x8123;
    std::memcpy(&data[2 * stride], &salt, 2);
    bool ok;
    const uint8_t* e = h3::DataElement(data.data(), (0x8123u << 16) | 2u, stride, &ok);
    CHECK(e == data.data() + 2 * stride && ok);
    e = h3::DataElement(data.data(), (0x8124u << 16) | 2u, stride, &ok);          // wrong salt: element returned, flagged
    CHECK(e == data.data() + 2 * stride && !ok);
    CHECK(h3::DataElement(data.data(), 0xFFFFFFFFu, stride, &ok) == nullptr && !ok);
    CHECK(h3::DataElement(nullptr, 1, stride, &ok) == nullptr);
    CHECK(h3::DataElement(data.data(), 1, 0, &ok) == nullptr);
}

static void TestCallSitesAndReattach() {
    uint8_t code[64] = {};
    code[10] = 0xE8;
    int32_t rel = 30 - (10 + 5);  // target = code + 30
    std::memcpy(code + 11, &rel, 4);
    CHECK(h3::CallTargets(code + 10, code + 30));
    CHECK(!h3::CallTargets(code + 10, code + 31));
    const uint8_t* sites[4];
    CHECK(h3::FindCallSites(code, sizeof(code), code + 30, sites, 4) == 1 && sites[0] == code + 10);
    CHECK(h3::DecideReattach(true, 0, 0) == h3::Reattach::Full);
    CHECK(h3::DecideReattach(false, 5, 0) == h3::Reattach::Full);
    CHECK(h3::DecideReattach(false, 5, 3) == h3::Reattach::DropChanged);
    CHECK(h3::DecideReattach(false, 5, 5) == h3::Reattach::None);
    CHECK(h3::DecideReattach(false, 0, 0) == h3::Reattach::None);
    CHECK(!h3::ShellRepeatDue(0) && !h3::ShellRepeatDue(63) && h3::ShellRepeatDue(64));
}

int main() {
    TestMsg();
    TestPathTail();
    TestClassify();
    TestWonAndErase();
    TestDataElement();
    TestCallSitesAndReattach();
    std::printf("test_h3_observe: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
