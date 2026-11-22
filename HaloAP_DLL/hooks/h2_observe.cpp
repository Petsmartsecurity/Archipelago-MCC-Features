#include "h2_observe.h"
#include "h2_observe_logic.h"
#include "../minhook/MinHook.h"
#include "../pattern_scan.h"
#include <intrin.h>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstring>

// Verified against halo2.dll (build in data/build_fingerprint.json) by tools/h2/h2_symbols.py (gate: h2-symbols) and
// tools/shared/check_mod_patterns.py (gate: hook-patterns). Evidence: docs/research/h2-base-hooks.md (QA corrections
// at its top win), docs/research/h2-kill-path.md 5.4/10, docs/stage-reports/phase8-h2-7a-qa.md.
// Observe only: every detour reads, formats, queues a message and calls the original exactly once.
namespace haloap {

    namespace {
        // ---- signatures (whole function prologues, address operands wildcarded) ---------------------------------
        // game_start(game_options*): post-hook; the primary load/reset event.
        const char* kGameStartPattern =
            "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B F1 E8 ? ? ? ? 48 83 3D ? ? ? ? 00 74 ? 80 3D ? ? ? ? "
            "00 74 ? E8 ? ? ? ? 84 C0 75 ? FF 15 ? ? ? ? E8 ? ? ? ? 48 8B";
        // game_start_for_map(game_options*) -> bool: its first call is game_start (load return address anchor).
        const char* kStartForMapPattern =
            "40 53 48 83 EC 20 48 8B D9 E8 ? ? ? ? E8 ? ? ? ? E8 ? ? ? ? E8 ? ? ? ? 48 8B D3 48 8B C8 E8 ? ? ? ? 0F "
            "B7 8B B0 02 00 00 E8 ? ? ? ? 84 C0 74 ? E8 ? ? ? ? E8 ? ?";
        // level_complete_notify(): once per game start in a campaign.
        const char* kCompletePattern =
            "48 83 EC 28 C6 05 ? ? ? ? 01 E8 ? ? ? ? 48 83 3D ? ? ? ? 00 74 ? E8 ? ? ? ? 84 C0 75 ? 48 83 C4 28 48 FF "
            "25 ? ? ? ? 48 83 C4 28 C3";
        // cinematic_set_title_impl(ushort index): chapter title.
        const char* kTitlePattern =
            "40 57 48 83 EC 20 48 8B 05 ? ? ? ? 0F B7 F9 48 85 C0 74 ? FF D0 84 C0 74 ? 48 8B 15 ? ? ? ? EB ? 48 8B "
            "15 ? ? ? ? 48 85 D2 74 ? 80 7A 0A 00 75 ? 48 89 5C 24 30 33 C0 0F 1F 00";
        // shell_message(int msg, void* ctx): host messages; ctx is never read here.
        const char* kShellPattern =
            "48 89 5C ? ? 57 48 83 EC 20 8D 41 FA 48 8B FA 8B D9 83 F8 01 77 ? E8 ? ? ? ? 84 C0 75 ? 83 FB 06 75 ? 38 "
            "05 ? ? ? ? 74 ? E8 ? ? ? ? 80 3D ? ? ? ? 00 0F 85 ? ? ? ? E8";
        // actor_died(actor handle): pins the actor size 0x8A0 (imul at the end) and the actor array load at +13.
        const char* kDiedPattern =
            "40 56 41 54 41 55 48 81 EC A0 00 00 00 48 8B 05 ? ? ? ? 45 33 E4 44 0F 29 54 24 30 48 89 BC 24 90 00 00 "
            "00 44 0F 29 5C 24 20 48 8B 50 48 44 8B E9 4C 8D 04 02 48 85 D2 75 ? 45 8B C4 41 0F B7 CD 48 69 F1 A0 08 "
            "00 00";
        // unit_die: only its call to actor_died is used (kill discriminator, not hooked).
        const char* kUnitDiePattern =
            "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 55 41 54 41 55 41 56 41 57 48 8D 68 98 48 81 EC 40 01 00 00 "
            "0F 29 70 C8 41 0F B6 D9 48 8B 05 ? ? ? ? 49 8B F8 8B F1 44 8B FA 48 8B 48 48 48 85 C9 74 ? 4C 8D 14 01 EB "
            "? 45 33 D2 0F B7 CE 4C 8D 34 49 4B 8D 0C B2 E8 ? ? ? ? 4C 8B E8 80 B8 AA 00 00 00 00 77 ? 44 0F B6 CB 4C "
            "8B C7";
        // cache_file_load_finish(): its first RIP load is the pointer to the current cache header.
        const char* kCachePattern =
            "40 55 57 48 83 EC 48 48 89 5C 24 60 48 8D 4C 24 68 48 8B 1D ? ? ? ? BD FF FF FF FF 48 89 74 24 70 40 32 "
            "F6 4C 89 64 24 40 4C 89 74 24 38 E8 ? ? ? ? 81 3B 64 61 65 68 44 8D 75 02 75 ?";
        // game_difficulty_get_real_impl(): loads the game globals pointer at +0.
        const char* kDifficultyPattern =
            "48 8B 05 ? ? ? ? 83 78 08 01 75 ? 0F B7 80 C6 02 00 00 C3 B8 01 00 00 00 C3";

        // RIP-relative loads inside the matched functions: (offset from function start, first 3 opcode bytes).
        struct RipLoad { size_t offset; uint8_t op[3]; };
        constexpr RipLoad kHdrFromCache         = { 0x11, { 0x48, 0x8B, 0x1D } };  // mov rbx,[cache header ptr]
        constexpr RipLoad kGlobalsFromDifficulty = { 0x0, { 0x48, 0x8B, 0x05 } };  // mov rax,[game globals]
        constexpr RipLoad kActorsFromDied       = { 0xD, { 0x48, 0x8B, 0x05 } };   // mov rax,[actor data array]

        // Call-site facts (checked at install, fail closed; re-verified offline by the gate).
        constexpr size_t kUnitDieCallOffset = 0x11F;      // E8 rel32 -> actor_died inside unit_die
        constexpr size_t kUnitDieKillRetOffset = 0x124;   // its return address = the kill discriminator
        constexpr size_t kStartCallOffset = 9;            // E8 rel32 -> game_start inside game_start_for_map
        constexpr size_t kStartRetOffset = 0xE;           // its return address = "load"
        constexpr size_t kGameStartCallSites = 2;         // game_start has exactly two direct callers (load, reset)

        // Struct fields read (V in the research docs; the signatures pin the actor size and the G/header loads).
        constexpr int kOptsMode = 0x00, kOptsMapId = 0xA4, kOptsPath = 0xA8, kOptsDifficulty = 0x2BE, kOptsPathChars = 260;
        constexpr int kHdrName = 0xB0;
        constexpr int kGMode = 0x08, kGMapId = 0xAC, kGDifficulty = 0x2C6, kGRunning = 0x1808, kGWon = 0x1818;
        constexpr int kActorUnit = 0x18, kActorTeam = 0x24, kActorSquad = 0x30, kActorPrevSquad = 0x34,
                      kActorStartLoc = 0x38, kActorChar = 0x54, kActorB7 = 0x07, kActorBd = 0x0D;
        constexpr uint32_t kActorSize = 0x8A0;

        // ---- hook table ----------------------------------------------------------------------------------------
        enum Pat { P_GAME_START, P_START_FOR_MAP, P_COMPLETE, P_TITLE, P_SHELL, P_DIED, P_UNIT_DIE, P_CACHE, P_DIFF, P_COUNT };
        constexpr int kHookCount = P_UNIT_DIE;  // the first six patterns are hook targets
        const char* const kPatName[P_COUNT] = { "game_start", "game_start_for_map", "level_complete", "cinematic_title",
                                                "shell_message", "actor_died", "unit_die", "cache_finish", "difficulty" };
        const char** const kPatText[P_COUNT] = { &kGameStartPattern, &kStartForMapPattern, &kCompletePattern, &kTitlePattern,
                                                 &kShellPattern, &kDiedPattern, &kUnitDiePattern, &kCachePattern,
                                                 &kDifficultyPattern };

        // ---- state ---------------------------------------------------------------------------------------------
        // Written by the worker thread before a hook is enabled, read by detours. Originals and targets are NEVER
        // nulled after uninstall: a game thread still inside a detour must always reach the original (QA m1).
        std::atomic<void*> g_orig[kHookCount];
        void* g_target[kHookCount] = {};
        bool g_on[kHookCount] = {};

        std::atomic<PipeClient*> g_pipe{ nullptr };
        std::atomic<uint32_t> g_seq{ 0 };
        std::atomic<uint32_t> g_dropped{ 0 };
        std::atomic<uintptr_t> g_base{ 0 };
        std::atomic<uintptr_t> g_size{ 0 };
        std::atomic<uintptr_t> g_hdrVar{ 0 }, g_gVar{ 0 }, g_actVar{ 0 };   // addresses of the game's pointer variables
        std::atomic<uintptr_t> g_loadRet{ 0 }, g_resetRet{ 0 }, g_killRet{ 0 };  // absolute return addresses (0 = unknown)

        struct ModId {
            uintptr_t base = 0;
            uint32_t stamp = 0, size = 0;
            bool operator==(const ModId& o) const { return base == o.base && stamp == o.stamp && size == o.size; }
        };
        // Worker-thread only:
        HMODULE g_mod = nullptr;     // module we attached to (identity below); nullptr = not attached
        ModId g_id;                  // identity at the last attach attempt
        int g_tickCount = 0;
        bool g_pinFailPrinted = false;

        // ---- SEH helpers: plain C functions, no objects with destructors ---------------------------------------
        bool ReadIdentity(HMODULE h, ModId* id) {
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

        // Every enabled hook must still start with MinHook's jmp (E9; EB when it patched above a short function).
        bool ProloguesIntact() {
            __try {
                for (int i = 0; i < kHookCount; i++) {
                    if (!g_on[i]) continue;
                    uint8_t b = *static_cast<const uint8_t*>(g_target[i]);
                    if (b != 0xE9 && b != 0xEB) return false;
                }
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // The RIP-relative load at fn+off must carry the expected opcode; returns the address of the variable it
        // reads, or 0 (fail closed) when the bytes differ or the target is outside the module image.
        uintptr_t ResolveRip(const uint8_t* fn, const RipLoad& load, uintptr_t base, uintptr_t size) {
            if (!fn) return 0;
            __try {
                const uint8_t* ins = fn + load.offset;
                if (memcmp(ins, load.op, 3) != 0) return 0;
                int32_t disp;
                memcpy(&disp, ins + 3, sizeof(disp));
                auto target = reinterpret_cast<uintptr_t>(ins + 7 + disp);
                return (target >= base && target < base + size) ? target : 0;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                return 0;
            }
        }

        bool CallAtTargets(const uint8_t* site, const uint8_t* target) {
            if (!site || !target) return false;
            __try {
                return h2::CallTargets(site, target);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // All E8 rel32 call sites to `target` in the executable sections. Returns the count (may exceed cap).
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
                    size_t n = h2::FindCallSites(base + va, size, target, sites, 8);
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

        // ---- reads done inside the detours (all under SEH) -------------------------------------------------------
        struct StartInfo {
            bool ok = false;
            int mode = -1, map = -1, diff = -1;
            char wpath[40] = {};
        };
        void ReadStart(const uint8_t* opts, StartInfo* out) {
            if (!opts) return;
            __try {
                out->mode = *reinterpret_cast<const int32_t*>(opts + kOptsMode);
                out->map = *reinterpret_cast<const int32_t*>(opts + kOptsMapId);
                out->diff = *reinterpret_cast<const int16_t*>(opts + kOptsDifficulty);
                uint16_t w[kOptsPathChars + 1];
                int n = 0;
                for (; n < kOptsPathChars; n++) {
                    w[n] = *reinterpret_cast<const uint16_t*>(opts + kOptsPath + 2 * n);
                    if (!w[n]) break;
                }
                w[n] = 0;
                h2::SanitizeWidePathTail(w, static_cast<size_t>(n), out->wpath, sizeof(out->wpath));
                out->ok = true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }

        enum CodeState { CODE_NONE, CODE_BAD, CODE_OK };
        // Mission code = cache header name (hdr+0xB0), only when the header carries its 'head' magic.
        CodeState ReadCode(char out[32]) {
            out[0] = 0;
            uintptr_t var = g_hdrVar.load(std::memory_order_relaxed);
            if (!var) return CODE_NONE;
            __try {
                const uint8_t* hdr = *reinterpret_cast<const uint8_t* const*>(var);
                if (!hdr) return CODE_NONE;
                if (*reinterpret_cast<const uint32_t*>(hdr) != h2::kHeaderMagic) return CODE_NONE;
                return h2::ValidCode(reinterpret_cast<const char*>(hdr) + kHdrName, out) ? CODE_OK : CODE_BAD;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                return CODE_NONE;
            }
        }

        struct GInfo {
            bool ok = false;
            int mode = -1, mapid = -1, diff = -1, running = -1, won = -1;
        };
        void ReadGlobals(GInfo* out) {
            uintptr_t var = g_gVar.load(std::memory_order_relaxed);
            if (!var) return;
            __try {
                const uint8_t* g = *reinterpret_cast<const uint8_t* const*>(var);
                if (!g) return;
                out->mode = *reinterpret_cast<const int32_t*>(g + kGMode);
                out->mapid = *reinterpret_cast<const int32_t*>(g + kGMapId);
                out->diff = *reinterpret_cast<const int16_t*>(g + kGDifficulty);
                out->running = *(g + kGRunning);
                out->won = *(g + kGWon);
                out->ok = true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }

        struct ActorInfo {
            bool ok = false;
            uint32_t unit = 0xFFFFFFFF, loc = 0, ch = 0;
            int team = -1, squad = -1, fsq = -1, b7 = -1, bd = -1;
        };
        void ReadActor(uint32_t handle, ActorInfo* out) {
            uintptr_t var = g_actVar.load(std::memory_order_relaxed);
            if (!var) return;
            __try {
                const uint8_t* arr = *reinterpret_cast<const uint8_t* const*>(var);
                const uint8_t* a = h2::DatumElement(arr, handle, kActorSize);  // m6: signature, size, index, salt
                if (!a) return;
                out->unit = *reinterpret_cast<const uint32_t*>(a + kActorUnit);
                out->team = *reinterpret_cast<const int16_t*>(a + kActorTeam);
                out->squad = *reinterpret_cast<const int32_t*>(a + kActorSquad);
                out->fsq = *reinterpret_cast<const int32_t*>(a + kActorPrevSquad);
                out->loc = *reinterpret_cast<const uint32_t*>(a + kActorStartLoc);
                out->ch = *reinterpret_cast<const uint32_t*>(a + kActorChar);
                out->b7 = *(a + kActorB7);
                out->bd = *(a + kActorBd);
                out->ok = true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                out->ok = false;
            }
        }

        // ---- message plumbing (game threads: a bounded snprintf and SendAsync, nothing else) --------------------
        h2::Msg NewMsg(const char* ev) {
            return h2::Msg(ev, g_seq.fetch_add(1, std::memory_order_relaxed) + 1, GetCurrentThreadId());
        }

        void Send(h2::Msg& m) {
            PipeClient* p = g_pipe.load(std::memory_order_acquire);
            if (!p || !p->IsConnected()) {
                g_dropped.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            uint32_t d = g_dropped.exchange(0, std::memory_order_relaxed);
            if (d) m.Add("drop=%u", d);
            p->SendAsync(m.Finish());
        }

        void AddRet(h2::Msg& m, const char* key, uintptr_t addr) {
            uintptr_t base = g_base.load(std::memory_order_relaxed), size = g_size.load(std::memory_order_relaxed);
            if (base && addr >= base && addr < base + size) m.Add("%s=%llx", key, static_cast<unsigned long long>(addr - base));
            else m.Add("%s=ext", key);
        }

        void AddCode(h2::Msg& m) {
            char code[32];
            CodeState cs = ReadCode(code);
            m.Add("code=%s", cs == CODE_OK ? code : cs == CODE_BAD ? "?" : "-");
        }

        template <class Fn>
        Fn Orig(int idx) { return reinterpret_cast<Fn>(g_orig[idx].load(std::memory_order_acquire)); }

        // ---- detours -------------------------------------------------------------------------------------------
        typedef void (*GameStartFn)(void*);
        typedef bool (*StartForMapFn)(void*);
        typedef void (*CompleteFn)();
        typedef void (*TitleFn)(uint16_t);
        typedef void (*ShellFn)(int, void*);
        typedef void (*DiedFn)(uint32_t);

        void DetourGameStart(void* opts) {
            const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
            Orig<GameStartFn>(P_GAME_START)(opts);  // post-hook: G is valid and the header is the new map afterwards
            StartInfo s;
            ReadStart(static_cast<const uint8_t*>(opts), &s);
            h2::StartKind kind = h2::ClassifyStart(ret, g_loadRet.load(std::memory_order_relaxed),
                                                   g_resetRet.load(std::memory_order_relaxed));
            h2::Msg m = NewMsg("game_start");
            m.Add("kind=%s", h2::StartKindName(kind));
            AddRet(m, "ret", ret);
            if (s.ok) m.Add("mode=%d map=%d diff=%d", s.mode, s.map, s.diff);
            else m.Add("mode=- map=- diff=-");
            AddCode(m);
            m.Add("wpath=%s", s.wpath[0] ? s.wpath : "-");
            Send(m);
        }

        bool DetourStartForMap(void* opts) {
            bool r = Orig<StartForMapFn>(P_START_FOR_MAP)(opts);
            h2::Msg m = NewMsg("load_result");
            m.Add("ok=%d", r ? 1 : 0);
            AddCode(m);
            Send(m);
            return r;
        }

        void DetourComplete() {
            const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
            GInfo g;
            ReadGlobals(&g);
            h2::Msg m = NewMsg("complete");
            AddCode(m);
            if (g.ok) m.Add("mode=%d mapid=%d diff=%d run=%d won=%d", g.mode, g.mapid, g.diff, g.running, g.won);
            else m.Add("mode=- mapid=- diff=- run=- won=-");
            AddRet(m, "ret", ret);
            Send(m);
            Orig<CompleteFn>(P_COMPLETE)();
        }

        void DetourTitle(uint16_t idx) {
            h2::Msg m = NewMsg("title");
            m.Add("idx=%u", static_cast<unsigned>(idx));
            AddCode(m);
            Send(m);
            Orig<TitleFn>(P_TITLE)(idx);
        }

        void DetourShell(int msg, void* ctx) {
            h2::Msg m = NewMsg("shell");  // ctx is deliberately not touched (QA: only ids 0x10-0x13 are safe to read)
            m.Add("msg=%d", msg);
            Send(m);
            Orig<ShellFn>(P_SHELL)(msg, ctx);
        }

        void DetourDied(uint32_t handle) {
            const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
            ActorInfo a;
            ReadActor(handle, &a);  // before: the original frees the actor
            GInfo g;
            ReadGlobals(&g);
            Orig<DiedFn>(P_DIED)(handle);
            h2::DeathKind kind = h2::ClassifyDeath(ret, g_killRet.load(std::memory_order_relaxed));
            h2::Msg m = NewMsg("died");
            m.Add("kind=%s", h2::DeathKindName(kind));
            AddRet(m, "src", ret);
            AddCode(m);
            m.Add("ah=%x", handle);
            if (a.ok) {
                m.Add("unit=%x team=%d squad=%d fsq=%d loc=%x ch=%x b7=%d bd=%d", a.unit, a.team, a.squad, a.fsq, a.loc,
                      a.ch, a.b7, a.bd);
            } else {
                m.Add("unit=- bad=1");
            }
            m.Add("diff=%d", g.ok ? g.diff : -1);
            Send(m);
        }

        void* const kDetours[kHookCount] = { reinterpret_cast<void*>(DetourGameStart),
                                             reinterpret_cast<void*>(DetourStartForMap),
                                             reinterpret_cast<void*>(DetourComplete), reinterpret_cast<void*>(DetourTitle),
                                             reinterpret_cast<void*>(DetourShell), reinterpret_cast<void*>(DetourDied) };

        // ---- install / uninstall (worker thread only) ----------------------------------------------------------
        // Holds a loader reference on halo2.dll for the scope: the module cannot be unloaded under a scan or a patch.
        // MCC's own FreeLibrary during that window just drops its count; the real unload then happens in our
        // destructor, on the worker thread, with no lock of ours held and outside MinHook's frozen-thread window.
        struct Pin {
            HMODULE h = nullptr;
            bool Acquire(HMODULE expect) {
                if (!GetModuleHandleExA(0, "halo2.dll", &h) || h != expect) {
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

        bool CreateAndEnable(int idx, void* target, bool sameBuild) {
            void* orig = nullptr;
            MH_STATUS s = MH_CreateHook(target, kDetours[idx], &orig);
            if (s == MH_ERROR_ALREADY_CREATED) {
                // A previous mapping of this module at the same address left an entry behind (MinHook cannot remove it
                // while the memory is unmapped). Only when the module is mapped again, pinned and the same build:
                // MH_RemoveHook restores the saved 5 bytes (identical) and frees the entry.
                if (!sameBuild) {
                    printf("[h2] %s: stale MinHook entry from a different build, hook off\n", kPatName[idx]);
                    return false;
                }
                MH_RemoveHook(target);
                s = MH_CreateHook(target, kDetours[idx], &orig);
            }
            if (s != MH_OK) {
                printf("[h2] %s: MH_CreateHook failed: %d (%s)\n", kPatName[idx], s, MH_StatusToString(s));
                return false;
            }
            g_orig[idx].store(orig, std::memory_order_release);  // before the jump exists
            s = MH_EnableHook(target);
            if (s != MH_OK) {
                printf("[h2] %s: MH_EnableHook failed: %d (%s)\n", kPatName[idx], s, MH_StatusToString(s));
                MH_RemoveHook(target);
                return false;
            }
            return true;
        }

        void AppendName(char* list, size_t cap, const char* s) {
            size_t n = strlen(list);
            if (n + 1 >= cap) return;
            snprintf(list + n, cap - n, "%s%s", n ? "," : "", s);
        }

        // Forget the hooks without touching the old module's memory (it is gone, replaced, or foreign).
        void ForgetModule() {
            for (int i = 0; i < kHookCount; i++) g_on[i] = false;  // g_orig / g_target stay valid on purpose
            g_hdrVar = g_gVar = g_actVar = 0;
            g_loadRet = g_resetRet = g_killRet = 0;
            g_mod = nullptr;
        }

        // Scans (only for hooks that are not installed) and installs. Returns false if the attempt could not start
        // (module vanished); true once an attempt was made, whatever its outcome.
        bool Attach(HMODULE h) {
            Pin pin;
            ModId id;
            if (!pin.Acquire(h) || !ReadIdentity(h, &id)) {
                if (!g_pinFailPrinted) printf("[h2] halo2.dll vanished while attaching; will retry\n");
                g_pinFailPrinted = true;
                return false;
            }
            g_pinFailPrinted = false;
            const bool sameBuild = g_id.stamp == id.stamp && g_id.size == id.size;  // at another base the old entries stay stale
            g_id = id;
            g_mod = h;
            const auto base = reinterpret_cast<const uint8_t*>(h);
            g_base = id.base;
            g_size = id.size;

            h2::Msg att = NewMsg("attach");
            att.Add("base=%llx stamp=%x size=%x", static_cast<unsigned long long>(id.base), id.stamp, id.size);
            Send(att);
            printf("[h2] halo2.dll at %p (stamp %08x, size %x)\n", h, id.stamp, id.size);

            // Resolve every signature: exactly one match in the executable sections or that item is off.
            const uint8_t* addr[P_COUNT] = {};
            int hits[P_COUNT] = {};
            for (int i = 0; i < P_COUNT; i++) {
                addr[i] = static_cast<const uint8_t*>(FindUniquePatternInModule(h, *kPatText[i], &hits[i]));
                if (!addr[i]) printf("[h2] signature %s: %s\n", kPatName[i], hits[i] ? "ambiguous (2+ matches)" : "not found");
            }

            // Anchors: RIP operands of signed functions, opcode bytes checked, inside the image.
            g_hdrVar = ResolveRip(addr[P_CACHE], kHdrFromCache, id.base, id.size);
            g_gVar = ResolveRip(addr[P_DIFF], kGlobalsFromDifficulty, id.base, id.size);
            g_actVar = ResolveRip(addr[P_DIED], kActorsFromDied, id.base, id.size);

            // Kill discriminator (M5): unit_die must contain E8 -> actor_died at +0x11F.
            if (addr[P_UNIT_DIE] && addr[P_DIED] &&
                CallAtTargets(addr[P_UNIT_DIE] + kUnitDieCallOffset, addr[P_DIED])) {
                g_killRet = reinterpret_cast<uintptr_t>(addr[P_UNIT_DIE]) + kUnitDieKillRetOffset;
            } else {
                printf("[h2] unit_die -> actor_died link not verified: deaths are reported as kind=unknown\n");
            }
            // Load return address: game_start_for_map's first call is game_start. Reset = the other caller of game_start.
            if (addr[P_START_FOR_MAP] && addr[P_GAME_START] &&
                CallAtTargets(addr[P_START_FOR_MAP] + kStartCallOffset, addr[P_GAME_START])) {
                const uint8_t* loadSite = addr[P_START_FOR_MAP] + kStartCallOffset;
                g_loadRet = reinterpret_cast<uintptr_t>(addr[P_START_FOR_MAP]) + kStartRetOffset;
                const uint8_t* sites[4] = {};
                size_t n = CallSitesInModule(h, addr[P_GAME_START], sites, 4);
                if (n == kGameStartCallSites) {
                    for (size_t k = 0; k < n; k++)
                        if (sites[k] != loadSite) g_resetRet = reinterpret_cast<uintptr_t>(sites[k]) + 5;
                } else {
                    printf("[h2] game_start has %zu direct callers (expected %zu): reset not classified\n", n, kGameStartCallSites);
                }
            } else {
                printf("[h2] game_start_for_map -> game_start link not verified: starts are reported as kind=other\n");
            }

            char on[200] = {}, off[240] = {};
            for (int i = 0; i < kHookCount; i++) {
                g_on[i] = false;
                if (!addr[i]) {
                    char entry[48];
                    snprintf(entry, sizeof(entry), "%s:%s", kPatName[i], hits[i] ? "ambig" : "none");
                    AppendName(off, sizeof(off), entry);
                    continue;
                }
                void* target = const_cast<uint8_t*>(addr[i]);
                g_target[i] = target;
                if (CreateAndEnable(i, target, sameBuild)) {
                    g_on[i] = true;
                    AppendName(on, sizeof(on), kPatName[i]);
                    printf("[h2] hook %s installed at RVA %llx\n", kPatName[i],
                           static_cast<unsigned long long>(addr[i] - base));
                } else {
                    char entry[48];
                    snprintf(entry, sizeof(entry), "%s:hookfail", kPatName[i]);
                    AppendName(off, sizeof(off), entry);
                }
            }

            h2::Msg ins = NewMsg("install");
            ins.Add("on=%s off=%s", on[0] ? on : "-", off[0] ? off : "-");
            ins.Add("hdr=%d g=%d act=%d kill=%d load=%d reset=%d", g_hdrVar.load() ? 1 : 0, g_gVar.load() ? 1 : 0,
                    g_actVar.load() ? 1 : 0, g_killRet.load() ? 1 : 0, g_loadRet.load() ? 1 : 0, g_resetRet.load() ? 1 : 0);
            Send(ins);
            return true;
        }

        void CheckIntegrity() {
            ModId id;
            bool bad = !ReadIdentity(g_mod, &id) || !(id == g_id);
            if (!bad && !ProloguesIntact()) bad = true;
            if (!bad) return;
            printf("[h2] halo2.dll identity or hook prologue changed: re-attaching\n");
            HMODULE h = g_mod;
            ForgetModule();
            h2::Msg m = NewMsg("reattach");
            Send(m);
            Attach(h);
        }
    }  // namespace

    void H2Tick(PipeClient* pipe) {
        g_pipe.store(pipe, std::memory_order_release);
        HMODULE cur = GetModuleHandleA("halo2.dll");  // no refcount: a compare only
        if (cur != g_mod) {
            if (g_mod) {
                printf("[h2] halo2.dll %s\n", cur ? "replaced" : "unloaded");
                ForgetModule();
                g_base = 0;
                h2::Msg m = NewMsg(cur ? "replaced" : "unload");
                Send(m);
            }
            g_tickCount = 0;
            if (cur) Attach(cur);
            return;
        }
        if (!cur) return;
        if (++g_tickCount >= 20) {  // ~2 s
            g_tickCount = 0;
            CheckIntegrity();
        }
    }

    void UninstallH2Observe() {
        if (g_mod) {
            Pin pin;
            ModId id;
            // Only touch memory when this is still the module we hooked.
            if (pin.Acquire(g_mod) && ReadIdentity(g_mod, &id) && id == g_id) {
                for (int i = kHookCount - 1; i >= 0; i--) {
                    if (g_on[i]) MH_RemoveHook(g_target[i]);  // disables first; the entry stays if it fails
                }
            }
            for (int i = 0; i < kHookCount; i++) g_on[i] = false;
            g_mod = nullptr;
        }
        g_pipe.store(nullptr, std::memory_order_release);  // detours copy the pointer once per message
    }

}  // namespace haloap
