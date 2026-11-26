#include "module_observer.h"
#include "../minhook/MinHook.h"
#include "../pattern_scan.h"
#include <cstdio>
#include <cstring>

// Shared lifecycle of the Halo 2 / Halo 3 (and later) observe-only hook sets. Moved out of h2_observe.cpp / h3_observe.cpp
// unchanged in behaviour and console / telemetry text; the QA history of that code is in
// docs/stage-reports/phase8-h2-7b-qa.md and phase8-h3-2-observe-qa.md. The one deliberate change is QA 8.H3.2 m5, see
// ForgetModule / RemoveHooks.
namespace haloap {
namespace obs {

    namespace {
        std::atomic<bool> g_observeLogging{ true };

        constexpr int kStaleBuild = 1000;  // CreateAndEnable status: stale MinHook entry from a different build
        constexpr int kNotAttempted = -1000;

        // ---- SEH helpers: plain C functions, no objects with destructors ----------------------------------------
        bool ReadIdentity(HMODULE h, ModId* id) {
            if (!h) return false;
            __try {
                auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(h);
                if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
                auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(h) + dos->e_lfanew);
                if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
                id->base = reinterpret_cast<uintptr_t>(h);
                id->stamp = nt->FileHeader.TimeDateStamp;
                id->size = nt->OptionalHeader.SizeOfImage;
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // Holds a loader reference on the game DLL for the scope. Used ONLY around MinHook create/enable/remove (MinHook
        // reads and patches without SEH). Scans and prints run unpinned under SEH (QA 8.H2.7b m4). MCC's own FreeLibrary
        // during the pin only drops its count; the real unload then happens in our destructor, on the worker thread,
        // with no lock of ours held and outside MinHook's freeze window.
        struct Pin {
            HMODULE h = nullptr;
            bool Acquire(const char* dll, HMODULE expect) {
                if (!GetModuleHandleExA(0, dll, &h) || h != expect) {
                    Release();
                    return false;
                }
                return true;
            }
            void Release() {
                if (h) { FreeLibrary(h); h = nullptr; }
            }
            ~Pin() { Release(); }
        };
    }  // namespace

    void SetObserveLogging(bool on) { g_observeLogging.store(on, std::memory_order_relaxed); }
    bool ObserveLoggingEnabled() { return g_observeLogging.load(std::memory_order_relaxed); }

    bool CallAtTargets(const uint8_t* site, const uint8_t* target) {
        if (!site || !target) return false;
        __try {
            return CallTargets(site, target);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    size_t CallSitesInModule(HMODULE h, const uint8_t* target, const uint8_t** out, size_t cap) {
        size_t total = 0;
        __try {
            auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(h);
            auto base = reinterpret_cast<const uint8_t*>(h);
            auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
            for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
                if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
                size_t va = sec->VirtualAddress;
                size_t size = sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData;
                if (va >= nt->OptionalHeader.SizeOfImage) continue;
                if (size > nt->OptionalHeader.SizeOfImage - va) size = nt->OptionalHeader.SizeOfImage - va;
                const uint8_t* sites[8];
                size_t n = FindCallSites(base + va, size, target, sites, 8);
                for (size_t k = 0; k < n && k < 8; k++) {
                    if (total + k < cap) out[total + k] = sites[k];
                }
                total += n;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
        return total;
    }

    // ---- message plumbing (game threads: a bounded snprintf and SendAsync, nothing else) ---------------------------
    Msg ModuleObserver::NewMsg(const char* ev) {
        Msg m(m_spec.tag, ev, m_seq.fetch_add(1, std::memory_order_relaxed) + 1, GetCurrentThreadId());
        if (m_spec.decorate) m_spec.decorate(m);
        return m;
    }

    void ModuleObserver::Send(Msg& m) {
        PipeClient* p = m_pipe.load(std::memory_order_acquire);
        if (!p || !p->IsConnected()) {
            m_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        uint32_t d = m_dropped.exchange(0, std::memory_order_relaxed);
        if (d) m.Add("drop=%u", d);
        p->SendAsync(m.Finish());
    }

    void ModuleObserver::AddRet(Msg& m, const char* key, uintptr_t addr) {
        uintptr_t base = m_base.load(std::memory_order_relaxed), size = m_size.load(std::memory_order_relaxed);
        if (base && addr >= base && addr < base + size) m.Add("%s=%llx", key, static_cast<unsigned long long>(addr - base));
        else m.Add("%s=ext", key);
    }

    // ---- install / uninstall (worker thread only) ----------------------------------------------------------------
    // Which enabled hooks still start with MinHook's jump (E9; EB when it patched above a short function).
    void ModuleObserver::CheckPrologues(bool* intact, int* enabled, int* intactCount) const {
        *enabled = 0;
        *intactCount = 0;
        for (int i = 0; i < m_spec.hookCount; i++) {
            intact[i] = false;
            if (!m_on[i]) continue;
            (*enabled)++;
            __try {
                uint8_t b = *static_cast<const uint8_t*>(m_target[i]);
                intact[i] = (b == 0xE9 || b == 0xEB);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                intact[i] = false;
            }
            if (intact[i]) (*intactCount)++;
        }
    }

    // No printing here (pinned). Returns MH_OK (0) or the failing MH status / kStaleBuild.
    int ModuleObserver::CreateAndEnable(int idx, void* target, bool sameBuild) {
        void* orig = nullptr;
        MH_STATUS s = MH_CreateHook(target, m_spec.detours[idx], &orig);
        if (s == MH_ERROR_ALREADY_CREATED) {
            // A previous mapping of this module at the same address left an entry behind that we did not get to forget
            // (a reload between two polls). Only when the module is mapped again, pinned and the same build:
            // MH_RemoveHook restores the saved 5 bytes (identical) and frees the entry.
            if (!sameBuild) return kStaleBuild;
            MH_RemoveHook(target);
            s = MH_CreateHook(target, m_spec.detours[idx], &orig);
        }
        if (s != MH_OK) return static_cast<int>(s);
        m_orig[idx].store(orig, std::memory_order_release);  // before the jump exists
        s = MH_EnableHook(target);
        if (s != MH_OK) {
            MH_RemoveHook(target);
            return static_cast<int>(s);
        }
        return MH_OK;
    }

    // QA 8.H3.2 m5 (applies to every observer): hooks of an image that is unmapped, replaced or re-attached can no longer be
    // restored, and MinHook keeps such entries "enabled" in its shared table: MH_Uninitialize would stop at the first one
    // (MH_ERROR_MEMORY_PROTECT) and, if anything else is mapped at that address, write the old bytes into it; a later hook at the
    // same address would take the ALREADY_CREATED path. So every installed hook is forgotten (entry dropped, no memory
    // written, trampoline leaked on purpose) whenever the module is forgotten.
    void ModuleObserver::ForgetModule() {
        for (int i = 0; i < m_spec.hookCount; i++) {
            if (m_on[i]) MH_ForgetHook(m_target[i]);
            m_on[i] = false;  // m_orig / m_target stay valid on purpose
        }
        for (int i = 0; i < m_spec.patCount; i++) { m_addr[i] = nullptr; m_hits[i] = 0; }
        m_spec.clearAnchors();
        m_retryStage = 0;
        m_mod = nullptr;
    }

    // Real removal only while this is still the module we hooked (identity checked under a pin); otherwise the image is gone
    // or foreign and nothing of it may be touched: its entries are forgotten instead.
    void ModuleObserver::RemoveHooks() {
        if (!m_mod) return;
        {
            Pin pin;
            ModId id;
            if (pin.Acquire(m_spec.dllName, m_mod) && ReadIdentity(m_mod, &id) && id == m_id) {
                for (int i = m_spec.hookCount - 1; i >= 0; i--) {
                    if (m_on[i] && MH_RemoveHook(m_target[i]) == MH_OK) m_on[i] = false;  // disables first; the entry stays if it fails
                }
            }
        }
        // whatever is still marked on could not be removed: forget it (ForgetModule does)
    }

    bool ModuleObserver::Incomplete() const {
        for (int i = 0; i < m_spec.hookCount; i++) if (!m_on[i]) return true;
        return m_spec.anchorsMissing();
    }

    // One scan/install pass. pass 0 = fresh attach (everything unknown); pass 1/2 = retry of what is still missing.
    // Never rescans a signature that is already resolved (installed prologues are patched, so they could not match).
    // Returns false if the pass could not run (module vanished or changed under it).
    bool ModuleObserver::Pass(HMODULE h, int pass) {
        const char* const tag = m_spec.logTag;
        const char* const dll = m_spec.dllName;
        ModId id;
        if (!ReadIdentity(h, &id)) return false;
        const ModId prev = m_id;
        if (pass == 0) {
            for (int i = 0; i < m_spec.patCount; i++) { m_addr[i] = nullptr; m_hits[i] = 0; }
        } else if (!(id == m_id)) {
            return false;
        }
        const auto base = reinterpret_cast<const uint8_t*>(h);
        char notes[320] = {};

        // 1. scan, unpinned (SEH inside FindUniquePatternInModule)
        for (int i = 0; i < m_spec.patCount; i++) {
            if (m_addr[i]) continue;
            m_addr[i] = static_cast<const uint8_t*>(FindUniquePatternInModule(h, *m_spec.patText[i], &m_hits[i]));
            if (!m_addr[i]) printf("[%s] signature %s: %s\n", tag, m_spec.patName[i], m_hits[i] ? "ambiguous (2+ matches)" : "not found");
        }
        m_spec.derive(DeriveCtx{ h, id, m_addr, notes, sizeof(notes) });

        // 2. the module must still be the one we read: nothing found above is used otherwise
        ModId again;
        if (GetModuleHandleA(dll) != h || !ReadIdentity(h, &again) || !(again == id)) {
            if (!m_pinFailPrinted) printf("[%s] %s changed while scanning; will retry\n", tag, dll);
            m_pinFailPrinted = true;
            if (pass == 0) { for (int i = 0; i < m_spec.patCount; i++) m_addr[i] = nullptr; }
            return false;
        }
        m_pinFailPrinted = false;
        const bool sameBuild = pass != 0 || (prev.stamp == id.stamp && prev.size == id.size);  // at another base the old entries stay stale
        if (pass == 0) {
            m_id = id;
            m_mod = h;
            m_base = id.base;
            m_size = id.size;
            m_attachTick = GetTickCount64();
            m_retryStage = 0;
            Msg att = NewMsg("attach");
            att.Add("base=%llx stamp=%x size=%x", static_cast<unsigned long long>(id.base), id.stamp, id.size);
            Send(att);
            printf("[%s] %s at %p (stamp %08x, size %x)\n", tag, dll, h, id.stamp, id.size);
        }
        if (notes[0]) printf("[%s] %s\n", tag, notes);

        // 3. pin only around MinHook create+enable, re-check identity inside the pin, no printing while pinned
        int status[kMaxHooks];
        bool pinned = false, need = false;
        for (int i = 0; i < m_spec.hookCount; i++) {
            status[i] = kNotAttempted;
            if (!m_on[i] && m_addr[i]) need = true;
        }
        if (need) {
            Pin pin;
            ModId inPin;
            pinned = pin.Acquire(dll, h) && ReadIdentity(h, &inPin) && inPin == id;
            if (pinned) {
                m_spec.commit();  // before any jump exists
                for (int i = 0; i < m_spec.hookCount; i++) {
                    if (m_on[i] || !m_addr[i]) continue;
                    void* target = const_cast<uint8_t*>(m_addr[i]);
                    m_target[i] = target;
                    status[i] = CreateAndEnable(i, target, sameBuild);
                    if (status[i] == MH_OK) m_on[i] = true;
                }
            }
        } else {
            m_spec.commit();
            pinned = true;
        }
        if (!pinned) {
            printf("[%s] could not pin %s or it changed; will retry\n", tag, dll);
            return false;
        }

        // 4. report (unpinned)
        char on[200] = {}, off[240] = {};
        for (int i = 0; i < m_spec.hookCount; i++) {
            if (m_on[i]) {
                AppendName(on, sizeof(on), m_spec.patName[i]);
                if (status[i] == MH_OK)
                    printf("[%s] hook %s installed at RVA %llx\n", tag, m_spec.patName[i], static_cast<unsigned long long>(m_addr[i] - base));
                continue;
            }
            char entry[48];
            if (!m_addr[i]) snprintf(entry, sizeof(entry), "%s:%s", m_spec.patName[i], m_hits[i] ? "ambig" : "none");
            else snprintf(entry, sizeof(entry), "%s:hookfail", m_spec.patName[i]);
            AppendName(off, sizeof(off), entry);
            if (status[i] == kStaleBuild) printf("[%s] %s: stale MinHook entry from a different build, hook off\n", tag, m_spec.patName[i]);
            else if (status[i] != kNotAttempted) printf("[%s] %s: MinHook failed: %d (%s)\n", tag, m_spec.patName[i], status[i], MH_StatusToString(static_cast<MH_STATUS>(status[i])));
        }
        m_spec.report();

        Msg ins = NewMsg("install");
        ins.Add("pass=%d on=%s off=%s", pass, on[0] ? on : "-", off[0] ? off : "-");
        m_spec.installFields(ins);
        Send(ins);
        return true;
    }

    void ModuleObserver::Attach(HMODULE h) {
        if (!Pass(h, 0)) m_mod = nullptr;  // retried on the next 100 ms slice
    }

    void ModuleObserver::CheckIntegrity() {
        const char* const tag = m_spec.logTag;
        ModId id;
        const bool changed = !ReadIdentity(m_mod, &id) || !(id == m_id);
        bool intact[kMaxHooks];
        int enabled, intactCount;
        CheckPrologues(intact, &enabled, &intactCount);
        switch (DecideReattach(changed, enabled, intactCount)) {
        case Reattach::None:
            return;
        case Reattach::DropChanged: {
            // Foreign change of some functions: keep the working hooks and every anchor, drop only the changed ones.
            char names[160] = {};
            for (int i = 0; i < m_spec.hookCount; i++) {
                if (m_on[i] && !intact[i]) {
                    m_on[i] = false;
                    m_addr[i] = nullptr;  // a later retry rescans it (and finds nothing while the foreign patch is there)
                    AppendName(names, sizeof(names), m_spec.patName[i]);
                }
            }
            printf("[%s] prologue of %s changed by something else: those hooks dropped, the rest kept\n", tag, names);
            Msg m = NewMsg("hookdrop");
            m.Add("dropped=%s", names);
            Send(m);
            return;
        }
        case Reattach::Full:
            break;
        }
        printf("[%s] %s identity changed or every hook prologue is gone: re-attaching\n", tag, m_spec.dllName);
        HMODULE h = m_mod;
        ForgetModule();
        Msg m = NewMsg("reattach");
        Send(m);
        Attach(h);
    }

    void ModuleObserver::Tick(PipeClient* pipe, HMODULE cur) {
        const char* const tag = m_spec.logTag;
        m_pipe.store(pipe, std::memory_order_release);
        if (!ObserveLoggingEnabled()) {
            // Off switch (launcher config observe_logging=0): never install; take down what a late setting message left behind.
            if (m_mod) {
                printf("[%s] observe logging is off: removing the %s hooks\n", tag, m_spec.dllName);
                RemoveHooks();
                ForgetModule();
                m_base = 0;
            }
            return;
        }
        const Presence pres = ClassifyPresence(reinterpret_cast<uintptr_t>(m_mod), reinterpret_cast<uintptr_t>(cur));
        if (pres == Presence::Attach || pres == Presence::Replaced || pres == Presence::Unloaded) {
            if (m_mod) {
                printf("[%s] %s %s\n", tag, m_spec.dllName, cur ? "replaced" : "unloaded");
                // Hooks of the old image: when it is gone MinHook cannot restore it (unmapped); RemoveHooks only touches
                // memory while the identity still matches, and ForgetModule drops whatever is left (QA 8.H3.2 m5).
                RemoveHooks();
                ForgetModule();
                m_base = 0;
                Msg m = NewMsg(cur ? "replaced" : "unload");
                Send(m);
            }
            m_tickCount = 0;
            if (cur) Attach(cur);
            return;
        }
        if (pres == Presence::Absent) return;
        if (IntegrityDue(&m_tickCount)) {  // ~2 s
            CheckIntegrity();
            if (!m_mod) return;
        }
        // Bounded retries of whatever is still missing (the loader may still have been working at attach time)
        if (m_retryStage < kRetryStages) {
            ULONGLONG elapsed = GetTickCount64() - m_attachTick;
            if (RetryDue(m_retryStage, elapsed)) {
                m_retryStage++;
                if (Incomplete()) Pass(m_mod, m_retryStage);
            }
        }
    }

    void ModuleObserver::Uninstall() {
        if (m_mod) {
            RemoveHooks();
            ForgetModule();  // forgets what could not be removed and clears the anchors (QA 8.H2.7b r3)
        }
        m_pipe.store(nullptr, std::memory_order_release);  // detours copy the pointer once per message
    }

}  // namespace obs
}  // namespace haloap
