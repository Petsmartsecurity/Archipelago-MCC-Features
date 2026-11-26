#pragma once
// Pure logic of the Halo 3 observe-only hook set (hooks/h3_observe.cpp): no game memory, no MinHook, no Windows,
// so mcc-ap/tests/test_h3_observe.cpp can run it on fake buffers. Shares the generic helpers of the Halo 2 set
// (call-site scan, reattach decision, shell coalescing, ASCII sanitising) instead of copying them.
#include "h2_observe_logic.h"

namespace haloap {
namespace h3 {

    using h2::kMsgMax;
    using h2::Reattach;
    using h2::DecideReattach;
    using h2::ShellRepeatDue;
    using h2::StartKind;
    using h2::ClassifyStart;
    using h2::StartKindName;
    using h2::CallTargets;
    using h2::FindCallSites;
    using h2::SafeChar;
    using h2::SanitizeAscii;

    constexpr const char* kTag = "H3";  // line prefix: "H3: ev=<name> n=<u> tid=<u> k=v ..."

    class Msg : public obs::Msg {
    public:
        Msg(const char* ev, uint32_t n, uint32_t tid) : obs::Msg(kTag, ev, n, tid) {}
    };

    // Last path component of a narrow string (separator '\\' or '/'), sanitised; maxChars bounds the read
    // (game_options+0x44 is char[0x104]). Used for the level path `levels\solo\<code>\<code>`.
    inline size_t SanitizePathTail(const char* s, size_t maxChars, char* out, size_t cap) {
        if (!cap) return 0;
        out[0] = 0;
        if (!s) return 0;
        size_t len = 0, tail = 0;
        while (len < maxChars && s[len]) {
            if (s[len] == '\\' || s[len] == '/') tail = len + 1;
            len++;
        }
        size_t n = 0;
        for (size_t i = tail; i < len && n + 1 < cap; i++) out[n++] = SafeChar(static_cast<unsigned char>(s[i]));
        out[n] = 0;
        return n;
    }

    // Who called main_game_change_immediate (PLAN 8.H3.2, QA 8.H3.1 m3). Addresses are absolute return addresses,
    // 0 = that link is not verified. A checkpoint "continue" reaches main_game_start through the same call as a fresh load,
    // so this is the only place the two can be told apart.
    enum class ChangeSrc { Queue, Checkpoint, Menu, Other };
    inline ChangeSrc ClassifyChangeSrc(uintptr_t ret, uintptr_t queueRet, uintptr_t ckptRet, uintptr_t menuRet) {
        if (queueRet && ret == queueRet) return ChangeSrc::Queue;
        if (ckptRet && ret == ckptRet) return ChangeSrc::Checkpoint;
        if (menuRet && ret == menuRet) return ChangeSrc::Menu;
        return ChangeSrc::Other;
    }
    inline const char* ChangeSrcName(ChangeSrc s) {
        return s == ChangeSrc::Queue ? "queue" : s == ChangeSrc::Checkpoint ? "checkpoint" : s == ChangeSrc::Menu ? "menu" : "other";
    }

    // game_options.mode: 1 campaign, 2 multiplayer, 3 main menu (V, docs/research/h3-base-hooks.md 2.1). A NULL options pointer or
    // mode 3 makes main_game_change_immediate return to the menu without starting a game: the quit signal.
    inline bool IsQuit(bool optsNull, int mode) { return optsNull || mode == 3; }

    // actor_died is reached from unit_die (a real death, ret == unitDieRet) and from the ai_kill actor path (a script kill,
    // ret == aiKillRet). Anything else, or an unverified link (0), is never claimed as a kill. Erases are a different function
    // (actor_delete with died == 0), reported as their own event.
    enum class DeathKind { Kill, ScriptKill, Unknown };
    inline DeathKind ClassifyDeath(uintptr_t ret, uintptr_t unitDieRet, uintptr_t aiKillRet) {
        if (unitDieRet && ret == unitDieRet) return DeathKind::Kill;
        if (aiKillRet && ret == aiKillRet) return DeathKind::ScriptKill;
        return DeathKind::Unknown;
    }
    inline const char* DeathKindName(DeathKind k) {
        return k == DeathKind::Kill ? "kill" : k == DeathKind::ScriptKill ? "script" : "unknown";
    }

    // game_won (0xf053c) has no once-only guard: the engine's level-won byte G[0xfb80] is 0 until the first call after a game
    // start. was < 0 = unreadable. `rep` counts the calls after the first one since it last was 0.
    struct WonDecision { bool first; bool log; };
    inline WonDecision DecideWon(int was, uint32_t repeatsSoFar, uint32_t repeatLogCap) {
        if (was == 0) return { true, true };
        return { false, repeatsSoFar < repeatLogCap };
    }

    // Erase events (actor_delete with died == 0) are numerous when a squad despawns: the first kEraseLogFirst of a game start are
    // logged one by one, then every kEraseLogEvery-th (the line carries the running count).
    constexpr uint32_t kEraseLogFirst = 400, kEraseLogEvery = 50;
    inline bool EraseLogDue(uint32_t count) { return count <= kEraseLogFirst || count % kEraseLogEvery == 0; }

    // Gen3 data array element (PLAN 8.H3.2): `data` = the array's element base (header +0x48), element = data + index * stride,
    // the first u16 of an element is its salt and must equal handle >> 16. Returns null for an invalid handle; *saltOk says
    // whether the salt matched (the caller still reads the element when it did not and flags it: the stride is R, not V).
    inline const uint8_t* DataElement(const uint8_t* data, uint32_t handle, uint32_t stride, bool* saltOk) {
        *saltOk = false;
        if (!data || handle == 0xFFFFFFFFu || !stride) return nullptr;
        const uint8_t* e = data + static_cast<size_t>(handle & 0xFFFF) * stride;
        uint16_t salt;
        memcpy(&salt, e, 2);
        *saltOk = salt == (handle >> 16);
        return e;
    }

}  // namespace h3
}  // namespace haloap
