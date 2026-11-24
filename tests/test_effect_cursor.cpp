// Plain C++ tests for effect_cursor.h (ADR-005): AP replays every item on every connect; each effect must reach the DLL once.
#include "effect_cursor.h"
#include "pipe_protocol.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace haloap;

static int g_checks = 0;
static int g_failures = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

int main() {
    CHECK(IsEffectItemId(81001) && IsEffectItemId(81999) && !IsEffectItemId(80999) && !IsEffectItemId(82000));
    CHECK(!IsEffectItemId(1) && !IsEffectItemId(101000) && !IsEffectItemId(90001));
    CHECK(EffectFileName("s", "a") == EffectFileName("s", "a"));
    CHECK(EffectFileName("s", "a") != EffectFileName("t", "a") && EffectFileName("s", "a") != EffectFileName("s", "b"));
    CHECK(EffectFileName("s", "a").rfind("haloap_effects_", 0) == 0);
    CHECK(EffectMessage(81001, 7) == "EFFECT: 81001 7");

    // memory only: new items forward once; a replay of the whole list forwards nothing
    {
        EffectCursor c;
        CHECK(c.OnItem(81001, 0));
        CHECK(c.OnItem(81011, 3));
        CHECK(!c.OnItem(81001, 0));            // AP resend after reconnect
        CHECK(!c.OnItem(81011, 3));
        CHECK(!c.OnItem(81002, 2));            // below the cursor and never seen: already handled in an earlier session
        CHECK(c.OnItem(81002, 4));
        CHECK(!c.OnItem(81001, -1));           // index -1: ignored
        CHECK(!c.OnItem(101000, 9));           // not an effect id
        CHECK(c.InFlight().size() == 3 && c.Cursor() == 4);
        CHECK(c.OnDone(81001, 0));
        CHECK(!c.OnDone(81001, 0));            // already done
        CHECK(!c.OnDone(81002, 3));            // id/index mismatch
        CHECK(c.InFlight().size() == 2 && c.InFlight()[0].index == 3);
        CHECK(!c.OnItem(81001, 0));            // done items never come back
    }

    // persistence: survives a restart; in-flight items are re-offered, applied ones are not
    {
        auto dir = std::filesystem::temp_directory_path() / "haloap_effect_cursor_test";
        std::filesystem::create_directories(dir);
        std::string a = (dir / "a.txt").string(), b = (dir / "b.txt").string();
        std::filesystem::remove(a); std::filesystem::remove(b);
        {
            EffectCursor c(a);
            CHECK(c.OnItem(81001, 0));
            CHECK(c.OnItem(81101, 1));
            CHECK(c.OnItem(81012, 2));
            CHECK(c.OnDone(81001, 0));
        }
        {
            EffectCursor c(a);                  // launcher restarted
            CHECK(c.Cursor() == 2);
            auto f = c.InFlight();
            CHECK(f.size() == 2 && f[0].index == 1 && f[0].id == 81101 && f[1].index == 2);
            CHECK(!c.OnItem(81001, 0) && !c.OnItem(81101, 1) && !c.OnItem(81012, 2));  // AP replay after the restart
            CHECK(c.OnItem(81002, 3));
        }
        {   // a different seed (different file) starts clean and does not touch the first
            EffectCursor c(a);
            c.Bind(b);
            CHECK(c.Cursor() == -1 && c.InFlight().empty());
            CHECK(c.OnItem(81001, 0));
            c.Bind(b);                          // same file again: state kept
            CHECK(c.InFlight().size() == 1);
            c.Bind(a);
            CHECK(c.Cursor() == 3 && c.InFlight().size() == 3);
        }
        {   // a damaged file: junk lines are ignored, the cursor never goes below a known in-flight index
            std::ofstream f(a, std::ios::trunc);
            f << "garbage\nCURSOR x\n5 81001\n7 99\n-1 81001\n";
        }
        {
            EffectCursor c(a);
            CHECK(c.Cursor() == 5 && c.InFlight().size() == 1);
            CHECK(!c.OnItem(81001, 5));
        }
        std::filesystem::remove(a); std::filesystem::remove(b);
    }

    // pipe messages from the DLL
    {
        auto ok = ParseEffectDone("EFFECT_APPLIED: 81001 12");
        CHECK(ok && ok->id == 81001 && ok->index == 12 && ok->applied);
        auto sk = ParseEffectDone("EFFECT_SKIPPED: 81104 0");
        CHECK(sk && sk->id == 81104 && sk->index == 0 && !sk->applied);
        CHECK(!ParseEffectDone("EFFECT_APPLIED: 81001"));
        CHECK(!ParseEffectDone("EFFECT_APPLIED: 81001 -2"));
        CHECK(!ParseEffectDone("EFFECT_APPLIED: 81001 1 2"));
        CHECK(!ParseEffectDone("EFFECT_APPLIED: 0 1"));
        CHECK(!ParseEffectDone("EFFECT_APPLIED:81001 1"));
        CHECK(!ParseEffectDone("EFFECT: 81001 1"));
        CHECK(!ParseEffectDone("EFFECT_APPLIED: 81001 99999999999"));
    }

    std::printf("effect cursor tests: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
