#pragma once
#include "../pipe_client.h"
#include "module_observer_logic.h"
#include <windows.h>
#include <atomic>
#include <cstddef>
#include <cstdint>

// Shared plumbing of the observe-only hook sets (Halo 2: h2_observe.cpp, Halo 3: h3_observe.cpp; later ODST / Reach / H4):
// module load / unload / replace detection, unique signature resolution, MinHook install with partial-install rollback,
// bounded retries, integrity check, re-attach, clean uninstall, telemetry sequence numbers and the pipe send.
// A game file owns its signature table, anchors, detours and what each line says; it describes them in an ObserverSpec.
// Lessons kept here (QA 8.H2.7b m2-m5/n1, 8.H3.2 m5): scan and print unpinned under SEH, pin only around MinHook
// create/enable/remove, originals and targets are never nulled, stale MinHook entries of an unmapped image are forgotten
// (MH_ForgetHook) instead of outliving it.
namespace haloap {
namespace obs {

    constexpr int kMaxPatterns = 24;
    constexpr int kMaxHooks = 16;

    // The switch for the whole feature (launcher config `observe_logging`, default on). Off = no observer installs a hook,
    // and a running one removes its hooks at the next tick.
    void SetObserveLogging(bool on);
    bool ObserveLoggingEnabled();

    // Worker-thread helpers for the games' derive callbacks (all under SEH).
    bool CallAtTargets(const uint8_t* site, const uint8_t* target);  // E8 rel32 at `site` reaches `target`
    // All E8 rel32 call sites to `target` in the executable sections; returns the count (may exceed cap).
    size_t CallSitesInModule(HMODULE h, const uint8_t* target, const uint8_t** out, size_t cap);

    struct DeriveCtx {
        HMODULE h;
        const ModId& id;
        const uint8_t* const* addr;  // resolved signature addresses by pattern index (null = not resolved)
        char* notes;                 // append " ... " sentences to print once ([tag] <notes>)
        size_t noteCap;
    };

    struct ObserverSpec {
        const char* tag;      // telemetry line prefix, "H2"
        const char* logTag;   // console prefix, "h2" -> "[h2] ..."
        const char* dllName;  // "halo2.dll"
        int patCount;         // all signatures (hook targets first, then anchors)
        int hookCount;        // the first hookCount patterns are hook targets
        const char* const* patName;
        const char** const* patText;  // patText[i] points at the signature string of pattern i
        void* const* detours;         // detours[i] for hook i
        // ---- game callbacks (worker thread only) ----
        void (*decorate)(Msg&);              // optional: fields added to every line (H2: tick=)
        void (*derive)(const DeriveCtx&);    // fill still-unknown anchors into the game's pending copy (seeded from the committed ones)
        void (*commit)();                    // publish the pending anchors (called before any jump exists)
        void (*clearAnchors)();              // zero the published anchors
        bool (*anchorsMissing)();            // any published anchor still 0
        void (*report)();                    // console warnings for missing anchors
        void (*installFields)(Msg&);         // anchor flags appended to the "install" line
    };

    class ModuleObserver {
    public:
        explicit ModuleObserver(const ObserverSpec& spec) : m_spec(spec) {}

        // Worker loop, every ~100 ms. `cur` = GetModuleHandleA(<dll>) (no refcount). Does nothing but a compare when idle.
        void Tick(PipeClient* pipe, HMODULE cur);
        // Removes the hooks when the module is still the one we hooked, forgets them when it is gone. Call before the pipe is freed.
        void Uninstall();

        // Game threads (detours): one bounded line, a SendAsync, nothing else.
        Msg NewMsg(const char* ev);
        void Send(Msg& m);
        void AddRet(Msg& m, const char* key, uintptr_t addr);  // module-relative RVA, or "ext"
        void* Orig(int idx) const { return m_orig[idx].load(std::memory_order_acquire); }

    private:
        bool Pass(HMODULE h, int pass);
        void Attach(HMODULE h);
        void CheckIntegrity();
        void RemoveHooks();
        void ForgetModule();
        bool Incomplete() const;
        int CreateAndEnable(int idx, void* target, bool sameBuild);
        void CheckPrologues(bool* intact, int* enabled, int* intactCount) const;

        const ObserverSpec& m_spec;

        // Detour side. Originals and targets are NEVER nulled after uninstall: a game thread still inside a detour must
        // always reach the original (QA 8.H2.7b m1).
        std::atomic<void*> m_orig[kMaxHooks] = {};
        void* m_target[kMaxHooks] = {};
        bool m_on[kMaxHooks] = {};
        std::atomic<PipeClient*> m_pipe{ nullptr };
        std::atomic<uint32_t> m_seq{ 0 };
        std::atomic<uint32_t> m_dropped{ 0 };
        std::atomic<uintptr_t> m_base{ 0 };
        std::atomic<uintptr_t> m_size{ 0 };

        // Worker thread only:
        HMODULE m_mod = nullptr;  // module we attached to (identity below); nullptr = not attached
        ModId m_id;
        int m_tickCount = 0;
        bool m_pinFailPrinted = false;
        const uint8_t* m_addr[kMaxPatterns] = {};
        int m_hits[kMaxPatterns] = {};
        int m_retryStage = 0;  // 0: none yet, 1: ~2 s retry done, 2: ~10 s retry done
        ULONGLONG m_attachTick = 0;
    };

}  // namespace obs
}  // namespace haloap
