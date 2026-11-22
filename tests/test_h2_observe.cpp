// Plain C++ tests for the pure logic of the Halo 2 observe hooks (hooks/h2_observe_logic.h) and the buffer scan
// of pattern_scan.cpp. No framework; exit code != 0 on failure.
#define NOMINMAX
#include "hooks/h2_observe_logic.h"
#include "pattern_scan.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace haloap;

static int g_checks = 0;
static int g_failures = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static size_t Count(const std::vector<uint8_t>& buf, const char* pat, size_t maxHits, size_t* first = nullptr) {
    ParsedPattern p = ParsePattern(pat);
    return CountMatches(buf.data(), buf.size(), p, maxHits, first);
}

static void TestPatternCount() {
    std::vector<uint8_t> b(64, 0x90);
    b[10] = 0x48; b[11] = 0x89; b[12] = 0x5C; b[13] = 0x24; b[14] = 0x08;
    size_t first = 0;
    CHECK(Count(b, "48 89 5C 24 08", 2, &first) == 1);  // unique
    CHECK(first == 10);
    CHECK(Count(b, "48 89 ? 24 08", 2) == 1);           // wildcard inside
    CHECK(Count(b, "? 89 5C 24 08", 2) == 1);           // leading wildcard: anchor is the first concrete byte
    CHECK(Count(b, "48 89 5C 24 09", 2) == 0);          // none
    b[40] = 0x48; b[41] = 0x89; b[42] = 0x5C; b[43] = 0x24; b[44] = 0x08;
    CHECK(Count(b, "48 89 5C 24 08", 2) == 2);          // ambiguous: counts to 2
    CHECK(Count(b, "48 89 5C 24 08", 1) == 1);          // stops at maxHits
    CHECK(Count(b, "48 89 5C 24 08", 5) == 2);
    // match flush against the end of the buffer, and one byte too short
    std::vector<uint8_t> e = { 1, 2, 3, 4, 5 };
    CHECK(Count(e, "03 04 05", 2) == 1);
    CHECK(Count(e, "04 05 06", 2) == 0);
    CHECK(Count(e, "01 02 03 04 05", 2) == 1);
    CHECK(Count(e, "01 02 03 04 05 06", 2) == 0);       // pattern longer than data
    CHECK(Count(e, "? ? ?", 2) == 0);                   // no concrete byte: matches nothing
    CHECK(Count(std::vector<uint8_t>(), "01", 2) == 0);
    // pattern whose first concrete byte occurs often (anchor retry loop)
    std::vector<uint8_t> r(100, 0xE8);
    r[99] = 0x01;
    CHECK(Count(r, "E8 E8 01", 2) == 1);
}

static void TestMsg() {
    h2::Msg m("died", 7, 1234);
    m.Add("kind=%s", "kill");
    m.Add("x=%d", -5);
    CHECK(std::strcmp(m.Finish(), "H2: ev=died n=7 tid=1234 kind=kill x=-5") == 0);
    CHECK(!m.truncated());

    // truncation: never more than 399 chars + NUL, still terminated
    h2::Msg big("big", 1, 2);
    for (int i = 0; i < 100; i++) big.Add("key%d=%s", i, "0123456789");
    CHECK(big.truncated());
    CHECK(std::strlen(big.Finish()) <= h2::kMsgMax - 1);
    CHECK(big.size() == h2::kMsgMax - 1);

    // one huge value
    std::string v(1000, 'a');
    h2::Msg one("one", 1, 2);
    one.Add("v=%s", v.c_str());
    CHECK(one.truncated());
    CHECK(std::strlen(one.Finish()) == h2::kMsgMax - 1);

    // Finish() replaces anything non-printable
    h2::Msg bad("bad", 1, 2);
    bad.Add("v=a\x01\x7F\xC3\xA9z");
    const char* s = bad.Finish();
    for (const char* p = s; *p; ++p) CHECK(static_cast<unsigned char>(*p) >= 0x20 && static_cast<unsigned char>(*p) <= 0x7E);
    CHECK(std::strstr(s, "v=a____z") != nullptr);
}

static void TestSanitize() {
    char out[16];
    CHECK(h2::SanitizeAscii("03a_oldmombasa", 100, out, sizeof(out)) == 14);
    CHECK(std::strcmp(out, "03a_oldmombasa") == 0);
    CHECK(h2::SanitizeAscii("a b\tc\xFF", 100, out, sizeof(out)) == 6);
    CHECK(std::strcmp(out, "a_b_c_") == 0);
    CHECK(h2::SanitizeAscii("abcdefghijklmnopqrstuvwxyz", 100, out, sizeof(out)) == 15);  // cap-1
    CHECK(h2::SanitizeAscii("abcdef", 3, out, sizeof(out)) == 3);                          // srcMax
    CHECK(std::strcmp(out, "abc") == 0);
    CHECK(h2::SanitizeAscii(nullptr, 5, out, sizeof(out)) == 0 && out[0] == 0);

    auto w = [](const wchar_t* s) { return std::vector<uint16_t>(s, s + wcslen(s) + 1); };
    char tail[24];
    auto v = w(L"scenarios\\solo\\03a_oldmombasa\\03a_oldmombasa");
    CHECK(h2::SanitizeWidePathTail(v.data(), v.size(), tail, sizeof(tail)) == 14);
    CHECK(std::strcmp(tail, "03a_oldmombasa") == 0);
    v = w(L"halo2/maps/x y.map");
    h2::SanitizeWidePathTail(v.data(), v.size(), tail, sizeof(tail));
    CHECK(std::strcmp(tail, "x_y.map") == 0);
    v = w(L"name\x00e9");  // non-ASCII (escape: the source file has no BOM)
    h2::SanitizeWidePathTail(v.data(), v.size(), tail, sizeof(tail));
    CHECK(std::strcmp(tail, "name_") == 0);
    v = w(L"dir\\");  // trailing separator: empty tail
    CHECK(h2::SanitizeWidePathTail(v.data(), v.size(), tail, sizeof(tail)) == 0);
    v = w(L"averyveryverylongmissionnamethatdoesnotfit");
    CHECK(h2::SanitizeWidePathTail(v.data(), v.size(), tail, sizeof(tail)) == sizeof(tail) - 1);
    // unterminated field: maxChars bounds the read
    std::vector<uint16_t> u(10, 'a');
    CHECK(h2::SanitizeWidePathTail(u.data(), u.size(), tail, sizeof(tail)) == 10);
    CHECK(h2::SanitizeWidePathTail(nullptr, 10, tail, sizeof(tail)) == 0);
}

static void TestValidCode() {
    char out[32];
    char f[32];
    std::memset(f, 0, sizeof(f));
    std::memcpy(f, "03a_oldmombasa", 15);
    CHECK(h2::ValidCode(f, out) && std::strcmp(out, "03a_oldmombasa") == 0);
    std::memset(f, 'a', 31);  // 31 chars + NUL: the longest allowed
    f[31] = 0;
    CHECK(h2::ValidCode(f, out) && std::strlen(out) == 31);
    std::memset(f, 'a', 32);  // no NUL inside the field
    CHECK(!h2::ValidCode(f, out) && out[0] == 0);
    std::memset(f, 0, sizeof(f));  // empty
    CHECK(!h2::ValidCode(f, out));
    std::memcpy(f, "03a old", 7);  // space
    CHECK(!h2::ValidCode(f, out));
    std::memset(f, 0, sizeof(f));
    f[0] = 'a'; f[1] = 0x01;
    CHECK(!h2::ValidCode(f, out));
    std::memset(f, 0, sizeof(f));
    f[0] = 'a'; f[1] = static_cast<char>(0xC3);
    CHECK(!h2::ValidCode(f, out));
    CHECK(!h2::ValidCode(nullptr, out));
}

static void TestClassify() {
    CHECK(h2::ClassifyStart(0x1000, 0x1000, 0x2000) == h2::StartKind::Load);
    CHECK(h2::ClassifyStart(0x2000, 0x1000, 0x2000) == h2::StartKind::Reset);
    CHECK(h2::ClassifyStart(0x3000, 0x1000, 0x2000) == h2::StartKind::Other);
    CHECK(h2::ClassifyStart(0, 0, 0) == h2::StartKind::Other);      // unresolved anchors never match a 0 return
    CHECK(h2::ClassifyStart(0x2000, 0x1000, 0) == h2::StartKind::Other);
    CHECK(std::strcmp(h2::StartKindName(h2::StartKind::Reset), "reset") == 0);

    CHECK(h2::ClassifyDeath(0x500, 0x500) == h2::DeathKind::Kill);
    CHECK(h2::ClassifyDeath(0x501, 0x500) == h2::DeathKind::Erase);
    CHECK(h2::ClassifyDeath(0x500, 0) == h2::DeathKind::Unknown);   // unverified link: never a kill
    CHECK(h2::ClassifyDeath(0, 0) == h2::DeathKind::Unknown);
    CHECK(std::strcmp(h2::DeathKindName(h2::DeathKind::Unknown), "unknown") == 0);
}

static void PutCall(std::vector<uint8_t>& b, size_t at, size_t target) {
    b[at] = 0xE8;
    int32_t rel = static_cast<int32_t>(static_cast<int64_t>(target) - static_cast<int64_t>(at + 5));
    std::memcpy(&b[at + 1], &rel, 4);
}

static void TestCallSites() {
    std::vector<uint8_t> b(256, 0x90);
    PutCall(b, 10, 200);
    PutCall(b, 50, 200);
    PutCall(b, 70, 100);          // a call to somewhere else
    const uint8_t* sites[4] = {};
    CHECK(h2::FindCallSites(b.data(), b.size(), b.data() + 200, sites, 4) == 2);
    CHECK(sites[0] == b.data() + 10 && sites[1] == b.data() + 50);
    CHECK(h2::CallTargets(b.data() + 10, b.data() + 200));
    CHECK(!h2::CallTargets(b.data() + 10, b.data() + 201));
    CHECK(!h2::CallTargets(b.data() + 11, b.data() + 200));          // not an E8
    CHECK(h2::FindCallSites(b.data(), b.size(), b.data() + 200, sites, 1) == 2);  // count may exceed cap
    // a call that ends exactly at the end of the buffer is found, one that would cross it is not read
    std::vector<uint8_t> e(32, 0x90);
    PutCall(e, 27, 0);
    CHECK(h2::FindCallSites(e.data(), e.size(), e.data(), sites, 4) == 1);
    e[28] = 0x90;                 // E8 at 27 with the rel32 now wrong, plus a stray E8 in the last 4 bytes
    e[29] = 0xE8;
    CHECK(h2::FindCallSites(e.data(), e.size(), e.data(), sites, 4) == 0);
    CHECK(h2::FindCallSites(e.data(), 4, e.data(), sites, 4) == 0);
}

// A fake data array: header (signature, max, element size, data offset) then elements with salts.
struct FakeArray {
    std::vector<uint8_t> mem;
    uint32_t elem, maxCount_;
    FakeArray(uint32_t elemSize, uint32_t maxCount) : mem(0x100 + static_cast<size_t>(elemSize) * maxCount, 0), elem(elemSize), maxCount_(maxCount) {
        uint32_t sig = h2::kDatumSignature;
        int32_t dataOff = 0x100;
        std::memcpy(&mem[h2::kDatumSigOff], &sig, 4);
        std::memcpy(&mem[h2::kDatumSizeOff], &elemSize, 4);
        std::memcpy(&mem[h2::kDatumMaxOff], &maxCount, 4);
        std::memcpy(&mem[h2::kDatumDataOff], &dataOff, 4);
    }
    void SetSalt(uint32_t idx, uint16_t salt) { std::memcpy(&mem[0x100 + static_cast<size_t>(idx) * elem], &salt, 2); }
};

static void TestDatum() {
    FakeArray a(0x8A0, 4);
    a.SetSalt(2, 0x1234);
    const uint8_t* e = h2::DatumElement(a.mem.data(), 0x12340002, 0x8A0);
    CHECK(e == a.mem.data() + 0x100 + 2 * 0x8A0);
    CHECK(h2::DatumElement(a.mem.data(), 0x12350002, 0x8A0) == nullptr);   // wrong salt
    CHECK(h2::DatumElement(a.mem.data(), 0x12340004, 0x8A0) == nullptr);   // index == max
    CHECK(h2::DatumElement(a.mem.data(), 0x1234FFFF, 0x8A0) == nullptr);   // index far out
    CHECK(h2::DatumElement(a.mem.data(), 0xFFFFFFFF, 0x8A0) == nullptr);   // null handle
    CHECK(h2::DatumElement(a.mem.data(), 0x12340002, 0x8A4) == nullptr);   // caller expects another element size
    CHECK(h2::DatumElement(nullptr, 0x12340002, 0x8A0) == nullptr);
    a.SetSalt(0, 0);
    CHECK(h2::DatumElement(a.mem.data(), 0x00000000, 0x8A0) == a.mem.data() + 0x100);  // salt 0 is a valid salt

    FakeArray b(0x8A0, 4);
    b.SetSalt(1, 7);
    uint32_t badSig = 0x12345678;
    std::memcpy(&b.mem[h2::kDatumSigOff], &badSig, 4);
    CHECK(h2::DatumElement(b.mem.data(), 0x00070001, 0x8A0) == nullptr);   // not a datum array
    std::memcpy(&b.mem[h2::kDatumSigOff], &h2::kDatumSignature, 4);
    CHECK(h2::DatumElement(b.mem.data(), 0x00070001, 0x8A0) != nullptr);
    int32_t zero = 0;
    std::memcpy(&b.mem[h2::kDatumDataOff], &zero, 4);                      // no data block
    CHECK(h2::DatumElement(b.mem.data(), 0x00070001, 0x8A0) == nullptr);
    int32_t zeroMax = 0;
    std::memcpy(&b.mem[h2::kDatumDataOff], "\x00\x01\x00\x00", 4);
    std::memcpy(&b.mem[h2::kDatumMaxOff], &zeroMax, 4);
    CHECK(h2::DatumElement(b.mem.data(), 0x00070001, 0x8A0) == nullptr);   // max count 0
}

int main() {
    TestPatternCount();
    TestMsg();
    TestSanitize();
    TestValidCode();
    TestClassify();
    TestCallSites();
    TestDatum();
    std::printf("test_h2_observe: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
