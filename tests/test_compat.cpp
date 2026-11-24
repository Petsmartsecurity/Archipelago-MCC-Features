// Plain C++ tests for compat.h: server address normalisation (QA B1) and seed compatibility (QA M2).
#include "compat.h"

#include <cstdio>

using namespace haloap;

static int g_checks = 0;
static int g_failures = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

int main() {
    // a typed scheme is always kept
    CHECK(NormalizeServerAddress("ws://friend.example.org:38281") == "ws://friend.example.org:38281");
    CHECK(NormalizeServerAddress("wss://archipelago.gg:12345") == "wss://archipelago.gg:12345");
    CHECK(NormalizeServerAddress("  ws://203.0.113.5:38281  ") == "ws://203.0.113.5:38281");
    // no scheme + remote: untouched (apclientpp tries wss then ws itself); never forced to wss://
    CHECK(NormalizeServerAddress("archipelago.gg:12345") == "archipelago.gg:12345");
    CHECK(NormalizeServerAddress("203.0.113.5:38281") == "203.0.113.5:38281");
    CHECK(NormalizeServerAddress("friend.example.org") == "friend.example.org");
    CHECK(NormalizeServerAddress("203.0.113.5:38281").find("wss://") == std::string::npos);
    // loopback without scheme: plain ws (a local server has no TLS)
    CHECK(NormalizeServerAddress("localhost:38281") == "ws://localhost:38281");
    CHECK(NormalizeServerAddress("127.0.0.1:38281") == "ws://127.0.0.1:38281");
    CHECK(NormalizeServerAddress("localhost") == "ws://localhost");
    CHECK(NormalizeServerAddress("[::1]:38281") == "ws://[::1]:38281");
    CHECK(NormalizeServerAddress("") == "");
    CHECK(NormalizeServerAddress("   ") == "");

    CHECK(CheckSeedCompat(-1) == SeedCompat::Missing);
    CHECK(CheckSeedCompat(kKillsanityVersion) == SeedCompat::Ok);
    CHECK(CheckSeedCompat(kKillsanityVersion + 1) == SeedCompat::Newer);
    CHECK(CheckSeedCompat(0) == SeedCompat::Ok);
    // the version sent in Connect must be at least the apworld's required_client_version (tests/test_compat_versions.py)
    static_assert(kClientVersionMajor == 1 && kClientVersionMinor == 2 && kClientVersionBuild == 10, "apworld required_client_version");

    std::printf("compat tests: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
