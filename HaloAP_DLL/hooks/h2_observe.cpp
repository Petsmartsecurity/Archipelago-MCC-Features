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
// at its top win), docs/research/h2-kill-path.md 5.4/10, docs/stage-reports/phase8-h2-7a-qa.md, phase8-h2-7b-qa.md.
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
        // skull_claim(uint n): hs ice_cream_flavor_stock(n), n = engine skull id 0..14 (PLAN 8.H2.9, docs/research/h2-skulls.md 3).
        const char* kSkullClaimPattern =
            "40 56 48 83 EC 50 48 63 F1 83 FE 0E 0F 87 ? ? ? ? 48 89 5C 24 60 48 89 7C 24 68 4C 89 74 24 70 0F 29 74 24 "
            "40 E8 ? ? ? ? 33 FF 4C 8D 35 ? ? ? ?";
        // claim_notify(player, n) -> bool: the MCC host unlock call. Two callers: skull_claim (script path, never for n 5) and the
        // event handler 0x73dd30 (IWHBYD, QA 8.H2.9 M1): the return address tells them apart.
        const char* kClaimNotifyPattern =
            "40 53 56 57 48 83 EC 30 48 63 DA 8B F9 E8 ? ? ? ? 48 8B F0 48 85 C0 0F 84 ? ? ? ? 4C 8B 00 48 8B C8 41 FF 50 50";
        // skull_apply_opts(settings*): applies the MCC skull mask (u64 at +0x48) to the engine at level start. Observed only.
        const char* kSkullApplyPattern =
            "40 53 48 83 EC 20 48 8B 59 48 F6 C3 01 74 0C B2 01 B9 0D 00 00 00 E8 ? ? ? ? F6 C3 02 74 0C B2 01 B9 02 00 "
            "00 00 E8 ? ? ? ?";
        // skull_active_query(id): not hooked; only its image-relative `movzx eax,[rbx+rdi+disp32]` locates the active array.
        const char* kSkullQueryPattern =
            "48 89 5C 24 08 57 48 83 EC 20 48 63 D9 83 FB 22 77 54 E8 ? ? ? ? 84 C0 75 0F 48 8B 05 ? ? ? ?";
        // unit_die: only its calls to actor_died (kill discriminator) and game_time_get (tick) are used; not hooked.
        const char* kUnitDiePattern =
            "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 55 41 54 41 55 41 56 41 57 48 8D 68 98 48 81 EC 40 01 00 00 "
            "0F 29 70 C8 41 0F B6 D9 48 8B 05 ? ? ? ? 49 8B F8 8B F1 44 8B FA 48 8B 48 48 48 85 C9 74 ? 4C 8D 14 01 EB "
            "? 45 33 D2 0F B7 CE 4C 8D 34 49 4B 8D 0C B2 E8 ? ? ? ? 4C 8B E8 80 B8 AA 00 00 00 00 77 ? 44 0F B6 CB 4C "
            "8B C7";
        // cache_file_load_finish(): its first RIP load is the pointer to the current cache header.
        const char* kCachePattern =
            "40 55 57 48 83 EC 48 48 89 5C 24 60 48 8D 4C 24 68 48 8B 1D ? ? ? ? BD FF FF FF FF 48 89 74 24 70 40 32 "
            "F6 4C 89 64 24 40 4C 89 74 24 38 E8 ? ? ? ? 81 3B 64 61 65 68 44 8D 75 02 75 ?";
        // game_difficulty_get_real_impl(): loads the game globals pointer at +0, reads +8 (mode) and +0x2C6 (difficulty).
        const char* kDifficultyPattern =
            "48 8B 05 ? ? ? ? 83 78 08 01 75 ? 0F B7 80 C6 02 00 00 C3 B8 01 00 00 00 C3";

        // RIP-relative loads inside the matched functions: (offset from function start, first 3 opcode bytes).
        struct RipLoad { size_t offset; uint8_t op[3]; };
        constexpr RipLoad kHdrFromCache         = { 0x11, { 0x48, 0x8B, 0x1D } };  // mov rbx,[cache header ptr]
        constexpr RipLoad kGlobalsFromDifficulty = { 0x0, { 0x48, 0x8B, 0x05 } };  // mov rax,[game globals]
        constexpr RipLoad kActorsFromDied       = { 0xD, { 0x48, 0x8B, 0x05 } };   // mov rax,[actor data array]
        // game_time_get (reached from unit_die, no signature): mov rax,[time globals]; mov eax,[rax+8]; ret
        constexpr RipLoad kTimeFromGameTime     = { 0x0, { 0x48, 0x8B, 0x05 } };
        constexpr uint8_t kTimeTail[4] = { 0x8B, 0x40, 0x08, 0xC3 };  // at offset 7; the 0x08 is kTimeTickOffset

        // Call-site facts (checked at install, fail closed; re-verified offline by the gate).
        constexpr size_t kUnitDieCallOffset = 0x11F;      // E8 rel32 -> actor_died inside unit_die
        constexpr size_t kUnitDieKillRetOffset = 0x124;   // its return address = the kill discriminator
        constexpr size_t kUnitDieTimeCallOffset = 0x12F;  // E8 rel32 -> game_time_get inside unit_die (tick anchor)
        constexpr size_t kStartCallOffset = 9;            // E8 rel32 -> game_start inside game_start_for_map
        constexpr size_t kStartRetOffset = 0xE;           // its return address = "load"
        constexpr size_t kGameStartCallSites = 2;
        // Skulls (docs/research/h2-skulls.md, QA 8.H2.9). skull_claim+0xF6 = E8 rel32 -> claim_notify (return = script path);
        // claim_notify has exactly two direct callers (that one and the IWHBYD event handler).
        constexpr size_t kSkullNotifyCallOffset = 0xF6;
        constexpr size_t kSkullNotifyRetOffset = 0xFB;
        constexpr size_t kClaimNotifyCallSites = 2;
        constexpr size_t kSkullClaimCallSites = 1;   // skull_claim: only the hs evaluator calls it, so it sees every script pickup
        constexpr size_t kSkullApplyCallSites = 1;   // skull_apply_opts: only the level-start path
        // skull_active_query+0x53: 0F B6 84 3B <disp32> = movzx eax,[rbx+rdi+disp32], rdi = image base: disp32 is the active array RVA.
        constexpr size_t kSkullArrOpOffset = 0x53;
        constexpr size_t kSkullArrDispOffset = 0x57;
        constexpr uint8_t kSkullArrOp[4] = { 0x0F, 0xB6, 0x84, 0x3B };
        constexpr int kSkullCount = 35;      // ids 0..34: pinned by `cmp ebx,0x22; ja` in kSkullQueryPattern
        constexpr int kApplyMaskOffset = 0x48;  // settings+0x48 u64: pinned by `mov rbx,[rcx+0x48]` in kSkullApplyPattern         // game_start has exactly two direct callers (load, reset)

        // Struct fields read (V in the research docs; the signatures pin the actor size and the G/header loads).
        constexpr int kOptsMode = 0x00, kOptsMapId = 0xA4, kOptsPath = 0xA8, kOptsDifficulty = 0x2BE, kOptsPathChars = 260;
        constexpr int kHdrName = 0xB0;
        constexpr int kGMode = 0x08, kGMapId = 0xAC, kGDifficulty = 0x2C6, kGRunning = 0x1808, kGWon = 0x1818;
        constexpr int kTimeTickOffset = 0x08;
        constexpr int kActorUnit = 0x18, kActorTeam = 0x24, kActorSquad = 0x30, kActorPrevSquad = 0x34,
                      kActorStartLoc = 0x38, kActorChar = 0x54, kActorB7 = 0x07, kActorBd = 0x0D;
        constexpr uint32_t kActorSize = 0x8A0;

        // ---- hook table (order of enum, kPatName, kPatText and kDetours must agree: the gate checks the source) -----
        enum Pat { P_GAME_START, P_START_FOR_MAP, P_COMPLETE, P_TITLE, P_SHELL, P_DIED, P_SKULL_CLAIM, P_CLAIM_NOTIFY, P_SKULL_APPLY,
                   P_UNIT_DIE, P_CACHE, P_DIFF, P_SKULL_QUERY, P_COUNT };
        constexpr int kHookCount = P_UNIT_DIE;  // the first nine patterns are hook targets
        const char* const kPatName[P_COUNT] = { "game_start", "game_start_for_map", "level_complete", "cinematic_title",
                                                "shell_message", "actor_died", "skull_claim", "claim_notify", "skull_apply_opts",
                                                "unit_die", "cache_finish", "difficulty", "skull_active_query" };
        const char** const kPatText[P_COUNT] = { &kGameStartPattern, &kStartForMapPattern, &kCompletePattern, &kTitlePattern,
                                                 &kShellPattern, &kDiedPattern, &kSkullClaimPattern, &kClaimNotifyPattern,
                                                 &kSkullApplyPattern, &kUnitDiePattern, &kCachePattern, &kDifficultyPattern,
                                                 &kSkullQueryPattern };

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
        // addresses of the game's pointer variables (header, game globals, actor array, time globals)
        std::atomic<uintptr_t> g_hdrVar{ 0 }, g_gVar{ 0 }, g_actVar{ 0 }, g_tickVar{ 0 };
        std::atomic<uintptr_t> g_loadRet{ 0 }, g_resetRet{ 0 }, g_killRet{ 0 };  // absolute return addresses (0 = unknown)
        std::atomic<uintptr_t> g_claimRet{ 0 };   // return address inside skull_claim after its claim_notify call (script path)
        std::atomic<uintptr_t> g_skullArr{ 0 };   // address of the engine's skull-active byte array (35 bytes), 0 = unknown
        // shell coalescing (exactness not required: atomics only)
        std::atomic<int32_t> g_lastShell{ INT32_MIN };
        std::atomic<uint32_t> g_shellRep{ 0 };

        struct ModId {
            uintptr_t base = 0;
            uint32_t stamp = 0, size = 0;
            bool operator==(const ModId& o) const { return base == o.base && stamp == o.stamp && size == o.size; }
        };
        // Worker-thread only:
        HMODULE g_mod = nullptr;     // module we attached to (identity below); nullptr = not attached
        ModId g_id;                  // identity at the last attach
        int g_tickCount = 0;
        bool g_pinFailPrinted = false;
        const uint8_t* g_addr[P_COUNT] = {};  // resolved signature addresses (null = not resolved)
        int g_hits[P_COUNT] = {};
        int g_retryStage = 0;                 // 0: none yet, 1: ~2 s retry done, 2: ~10 s retry done
        ULONGLONG g_attachTick = 0;

        // ---- SEH helpers: plain C functions, no objects with destructors ---------------------------------------
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

        // Which enabled hooks still start with MinHook's jump (E9; EB when it patched above a short function).
        void CheckPrologues(bool* intact, int* enabled, int* intactCount) {
            *enabled = 0;
            *intactCount = 0;
            for (int i = 0; i < kHookCount; i++) {
                intact[i] = false;
                if (!g_on[i]) continue;
                (*enabled)++;
                __try {
                    uint8_t b = *static_cast<const uint8_t*>(g_target[i]);
                    intact[i] = (b == 0xE9 || b == 0xEB);
                }
                __except (EXCEPTION_EXECUTE_HANDLER) {
                    intact[i] = false;
                }
                if (intact[i]) (*intactCount)++;
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

        // game_time_get: unit_die+0x12F must be E8 to a function that is exactly `mov rax,[rip+d]; mov eax,[rax+8]; ret`.
        // Returns the address of the time-globals variable, or 0.
        uintptr_t ResolveTickVar(const uint8_t* unitDie, uintptr_t base, uintptr_t size) {
            if (!unitDie) return 0;
            const uint8_t* fn = nullptr;
            __try {
                const uint8_t* site = unitDie + kUnitDieTimeCallOffset;
                if (site[0] != 0xE8) return 0;
                int32_t rel;
                memcpy(&rel, site + 1, sizeof(rel));
                fn = site + 5 + rel;
                if (memcmp(fn + 7, kTimeTail, sizeof(kTimeTail)) != 0) return 0;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                return 0;
            }
            return ResolveRip(fn, kTimeFromGameTime, base, size);
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

        // Game tick (QA 7b m1): a tick that goes backwards within one game_start = a checkpoint revert.
        bool ReadTick(int* out) {
            uintptr_t var = g_tickVar.load(std::memory_order_relaxed);
            if (!var) return false;
            __try {
                const uint8_t* t = *reinterpret_cast<const uint8_t* const*>(var);
                if (!t) return false;
                *out = *reinterpret_cast<const int32_t*>(t + kTimeTickOffset);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
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

        // image-relative `movzx eax,[rbx+rdi+disp32]` (rdi = module base): returns base + disp32, or 0 (fail closed) when the
        // opcode bytes differ or the target is outside the image.
        uintptr_t ResolveImageRel(const uint8_t* fn, uintptr_t base, uintptr_t size) {
            if (!fn) return 0;
            __try {
                if (memcmp(fn + kSkullArrOpOffset, kSkullArrOp, sizeof(kSkullArrOp)) != 0) return 0;
                int32_t disp;
                memcpy(&disp, fn + kSkullArrDispOffset, sizeof(disp));
                if (disp <= 0 || static_cast<uint32_t>(disp) + kSkullCount > size) return 0;
                return base + static_cast<uint32_t>(disp);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                return 0;
            }
        }

        // Read-only snapshot of the engine's active skulls as a bit mask over engine ids (bit id set = active byte != 0).
        // false = array unknown or unreadable. Never writes.
        bool ReadActiveSkulls(uint64_t* out) {
            uintptr_t var = g_skullArr.load(std::memory_order_relaxed);
            if (!var) return false;
            __try {
                *out = h2::ActiveMask(reinterpret_cast<const uint8_t*>(var), kSkullCount);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // The MCC skull bit mask the engine is about to be given (settings+0x48).
        bool ReadMccMask(const uint8_t* settings, uint64_t* out) {
            if (!settings) return false;
            __try {
                *out = *reinterpret_cast<const uint64_t*>(settings + kApplyMaskOffset);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // ---- message plumbing (game threads: a bounded snprintf and SendAsync, nothing else) --------------------
        h2::Msg NewMsg(const char* ev) {
            h2::Msg m(ev, g_seq.fetch_add(1, std::memory_order_relaxed) + 1, GetCurrentThreadId());
            int tick;
            if (ReadTick(&tick)) m.Add("tick=%d", tick);
            else m.Add("tick=-");
            return m;
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
        typedef void (*SkullClaimFn)(uint32_t);
        typedef bool (*ClaimNotifyFn)(int, int);
        typedef void (*SkullApplyFn)(void*);

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

        void SendShell(int msg, uintptr_t ret, uint32_t rep, bool flush) {
            h2::Msg m = NewMsg("shell");
            m.Add("msg=%d", msg);
            if (ret) AddRet(m, "ret", ret);
            if (rep) m.Add("rep=%u", rep);
            if (flush) m.Add("flush=1");
            Send(m);
        }

        void DetourShell(int msg, void* ctx) {
            const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());  // both call sites are tail jumps: the host caller
            // ctx is deliberately not touched (QA: only ids 0x10-0x13 are safe to read).
            // Coalescing: the first of a run is sent, then one line per 64 repeats (rep = repeats so far), and the count is
            // flushed when the id changes. Atomics only; a race can lose or duplicate a line, never block.
            // Pause, quit, revert and restart are the ids the session must see one by one (QA 7b r1): never coalesced.
            const bool always = msg == 0 || msg == 2 || msg == 3 || msg == 4 || msg == 0xD;
            int32_t prev = g_lastShell.exchange(msg, std::memory_order_relaxed);
            if (always) {
                uint32_t rep = g_shellRep.exchange(0, std::memory_order_relaxed);
                if (rep && prev != INT32_MIN && prev != msg) SendShell(prev, 0, rep, true);
                SendShell(msg, ret, 0, false);
            } else if (prev != msg) {
                uint32_t rep = g_shellRep.exchange(0, std::memory_order_relaxed);
                if (rep && prev != INT32_MIN) SendShell(prev, 0, rep, true);
                SendShell(msg, ret, 0, false);
            } else {
                uint32_t c = g_shellRep.fetch_add(1, std::memory_order_relaxed) + 1;
                if (h2::ShellRepeatDue(c)) SendShell(msg, ret, c, false);
            }
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

        // Skull observers (PLAN 8.H2.9b). Each calls the original FIRST and exactly once, then reads/formats/sends: nothing here
        // writes game memory or changes an argument or return value. Forcing skulls is 8.H2.9c.
        void DetourSkullClaim(uint32_t n) {
            const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
            Orig<SkullClaimFn>(P_SKULL_CLAIM)(n);
            GInfo g;
            ReadGlobals(&g);
            h2::Msg m = NewMsg("skull");
            m.Add("path=claim n=%u", n);
            AddRet(m, "src", ret);
            AddCode(m);
            m.Add("diff=%d", g.ok ? g.diff : -1);
            Send(m);
        }

        bool DetourClaimNotify(int player, int n) {
            const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
            bool r = Orig<ClaimNotifyFn>(P_CLAIM_NOTIFY)(player, n);
            GInfo g;
            ReadGlobals(&g);
            h2::Msg m = NewMsg("skull");
            m.Add("path=%s n=%d plr=%d", h2::ClaimPathName(h2::ClassifyClaim(ret, g_claimRet.load(std::memory_order_relaxed))), n, player);
            AddRet(m, "src", ret);
            AddCode(m);
            m.Add("diff=%d", g.ok ? g.diff : -1);
            Send(m);
            return r;
        }

        // pre = the active array before the original (stale state from the previous start: QA M2), mask = what MCC hands the
        // engine, act = the array after.
        void DetourSkullApply(void* settings) {
            uint64_t pre = 0, act = 0, mask = 0;
            const bool havePre = ReadActiveSkulls(&pre);
            const bool haveMask = ReadMccMask(static_cast<const uint8_t*>(settings), &mask);
            Orig<SkullApplyFn>(P_SKULL_APPLY)(settings);
            const bool haveAct = ReadActiveSkulls(&act);
            GInfo g;
            ReadGlobals(&g);
            h2::Msg m = NewMsg("skull_apply");
            if (haveMask) m.Add("mask=%llx", static_cast<unsigned long long>(mask)); else m.Add("mask=-");
            if (havePre) m.Add("pre=%llx", static_cast<unsigned long long>(pre)); else m.Add("pre=-");
            if (haveAct) m.Add("act=%llx", static_cast<unsigned long long>(act)); else m.Add("act=-");
            m.Add("mode=%d diff=%d", g.ok ? g.mode : -1, g.ok ? g.diff : -1);
            AddCode(m);
            Send(m);
        }

        void* const kDetours[kHookCount] = { reinterpret_cast<void*>(DetourGameStart),
                                             reinterpret_cast<void*>(DetourStartForMap),
                                             reinterpret_cast<void*>(DetourComplete), reinterpret_cast<void*>(DetourTitle),
                                             reinterpret_cast<void*>(DetourShell), reinterpret_cast<void*>(DetourDied),
                                             reinterpret_cast<void*>(DetourSkullClaim),
                                             reinterpret_cast<void*>(DetourClaimNotify),
                                             reinterpret_cast<void*>(DetourSkullApply) };

        // ---- install / uninstall (worker thread only) ----------------------------------------------------------
        // Holds a loader reference on halo2.dll for the scope. Used ONLY around MinHook create/enable/remove (MinHook
        // reads and patches without SEH). Scans and prints run unpinned under SEH (QA 7b m4). MCC's own FreeLibrary
        // during the pin only drops its count; the real unload then happens in our destructor, on the worker thread,
        // with no lock of ours held and outside MinHook's freeze window.
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

        constexpr int kStaleBuild = 1000;  // CreateAndEnable status: stale MinHook entry from a different build
        constexpr int kNotAttempted = -1000;

        // No printing here (pinned). Returns MH_OK (0) or the failing MH status / kStaleBuild.
        int CreateAndEnable(int idx, void* target, bool sameBuild) {
            void* orig = nullptr;
            MH_STATUS s = MH_CreateHook(target, kDetours[idx], &orig);
            if (s == MH_ERROR_ALREADY_CREATED) {
                // A previous mapping of this module at the same address left an entry behind (MinHook cannot remove it
                // while the memory is unmapped). Only when the module is mapped again, pinned and the same build:
                // MH_RemoveHook restores the saved 5 bytes (identical) and frees the entry.
                if (!sameBuild) return kStaleBuild;
                MH_RemoveHook(target);
                s = MH_CreateHook(target, kDetours[idx], &orig);
            }
            if (s != MH_OK) return static_cast<int>(s);
            g_orig[idx].store(orig, std::memory_order_release);  // before the jump exists
            s = MH_EnableHook(target);
            if (s != MH_OK) {
                MH_RemoveHook(target);
                return static_cast<int>(s);
            }
            return MH_OK;
        }

        void AppendName(char* list, size_t cap, const char* s) {
            size_t n = strlen(list);
            if (n + 1 >= cap) return;
            snprintf(list + n, cap - n, "%s%s", n ? "," : "", s);
        }

        // Forget everything without touching the old module's memory (it is gone, replaced, or foreign).
        void ForgetModule() {
            for (int i = 0; i < kHookCount; i++) g_on[i] = false;  // g_orig / g_target stay valid on purpose
            for (int i = 0; i < P_COUNT; i++) { g_addr[i] = nullptr; g_hits[i] = 0; }
            g_hdrVar = 0; g_gVar = 0; g_actVar = 0; g_tickVar = 0;
            g_loadRet = 0; g_resetRet = 0; g_killRet = 0; g_claimRet = 0; g_skullArr = 0;
            g_retryStage = 0;
            g_mod = nullptr;
        }

        struct Derived {
            uintptr_t hdrVar = 0, gVar = 0, actVar = 0, tickVar = 0, killRet = 0, loadRet = 0, resetRet = 0, claimRet = 0, skullArr = 0;
        };

        // Fills only the fields that are still 0, from the resolved signature addresses (installed targets included:
        // the anchors sit beyond the 5 patched bytes). Everything under SEH; nothing is committed here.
        void ComputeDerived(HMODULE h, const ModId& id, Derived* d, char* notes, size_t noteCap) {
            if (!d->hdrVar) d->hdrVar = ResolveRip(g_addr[P_CACHE], kHdrFromCache, id.base, id.size);
            if (!d->gVar) d->gVar = ResolveRip(g_addr[P_DIFF], kGlobalsFromDifficulty, id.base, id.size);
            if (!d->actVar) d->actVar = ResolveRip(g_addr[P_DIED], kActorsFromDied, id.base, id.size);
            if (!d->tickVar) d->tickVar = ResolveTickVar(g_addr[P_UNIT_DIE], id.base, id.size);
            // Kill discriminator (M5): unit_die must contain E8 -> actor_died at +0x11F.
            if (!d->killRet && g_addr[P_UNIT_DIE] && g_addr[P_DIED] &&
                CallAtTargets(g_addr[P_UNIT_DIE] + kUnitDieCallOffset, g_addr[P_DIED])) {
                d->killRet = reinterpret_cast<uintptr_t>(g_addr[P_UNIT_DIE]) + kUnitDieKillRetOffset;
            }
            // Skull script path: skull_claim must contain E8 -> claim_notify at +0xF6; its return address marks the script caller.
            if (!d->claimRet && g_addr[P_SKULL_CLAIM] && g_addr[P_CLAIM_NOTIFY] &&
                CallAtTargets(g_addr[P_SKULL_CLAIM] + kSkullNotifyCallOffset, g_addr[P_CLAIM_NOTIFY])) {
                d->claimRet = reinterpret_cast<uintptr_t>(g_addr[P_SKULL_CLAIM]) + kSkullNotifyRetOffset;
            }
            if (!d->skullArr) d->skullArr = ResolveImageRel(g_addr[P_SKULL_QUERY], id.base, id.size);
            // Load return address: game_start_for_map's first call is game_start. Reset = the other caller of game_start.
            if (!d->loadRet && g_addr[P_START_FOR_MAP] && g_addr[P_GAME_START] &&
                CallAtTargets(g_addr[P_START_FOR_MAP] + kStartCallOffset, g_addr[P_GAME_START])) {
                d->loadRet = reinterpret_cast<uintptr_t>(g_addr[P_START_FOR_MAP]) + kStartRetOffset;
            }
            if (d->loadRet && !d->resetRet) {
                const uint8_t* loadSite = g_addr[P_START_FOR_MAP] + kStartCallOffset;
                const uint8_t* sites[4] = {};
                size_t n = CallSitesInModule(h, g_addr[P_GAME_START], sites, 4);
                if (n == kGameStartCallSites) {
                    for (size_t k = 0; k < n; k++)
                        if (sites[k] != loadSite) d->resetRet = reinterpret_cast<uintptr_t>(sites[k]) + 5;
                } else {
                    snprintf(notes + strlen(notes), noteCap - strlen(notes), "game_start has %zu direct callers (expected %zu): reset not classified. ", n, kGameStartCallSites);
                }
            }
        }

        bool Incomplete() {
            for (int i = 0; i < kHookCount; i++) if (!g_on[i]) return true;
            return !g_hdrVar.load() || !g_gVar.load() || !g_actVar.load() || !g_tickVar.load() || !g_killRet.load() ||
                   !g_loadRet.load() || !g_resetRet.load() || !g_claimRet.load() || !g_skullArr.load();
        }

        // One scan/install pass. pass 0 = fresh attach (everything unknown); pass 1/2 = retry of what is still missing.
        // Never rescans a signature that is already resolved (installed prologues are patched, so they could not match).
        // Returns false if the pass could not run (module vanished or changed under it).
        bool Pass(HMODULE h, int pass) {
            ModId id;
            if (!ReadIdentity(h, &id)) return false;
            const ModId prev = g_id;
            if (pass == 0) {
                for (int i = 0; i < P_COUNT; i++) { g_addr[i] = nullptr; g_hits[i] = 0; }
            } else if (!(id == g_id)) {
                return false;
            }
            const auto base = reinterpret_cast<const uint8_t*>(h);
            char notes[320] = {};

            // 1. scan, unpinned (SEH inside FindUniquePatternInModule)
            for (int i = 0; i < P_COUNT; i++) {
                if (g_addr[i]) continue;
                g_addr[i] = static_cast<const uint8_t*>(FindUniquePatternInModule(h, *kPatText[i], &g_hits[i]));
                if (!g_addr[i]) printf("[h2] signature %s: %s\n", kPatName[i], g_hits[i] ? "ambiguous (2+ matches)" : "not found");
            }
            Derived d;
            d.hdrVar = g_hdrVar; d.gVar = g_gVar; d.actVar = g_actVar; d.tickVar = g_tickVar;
            d.killRet = g_killRet; d.loadRet = g_loadRet; d.resetRet = g_resetRet; d.claimRet = g_claimRet; d.skullArr = g_skullArr;
            ComputeDerived(h, id, &d, notes, sizeof(notes));

            // 2. the module must still be the one we read: nothing found above is used otherwise
            ModId again;
            if (GetModuleHandleA("halo2.dll") != h || !ReadIdentity(h, &again) || !(again == id)) {
                if (!g_pinFailPrinted) printf("[h2] halo2.dll changed while scanning; will retry\n");
                g_pinFailPrinted = true;
                if (pass == 0) { for (int i = 0; i < P_COUNT; i++) g_addr[i] = nullptr; }
                return false;
            }
            g_pinFailPrinted = false;
            const bool sameBuild = pass != 0 || (prev.stamp == id.stamp && prev.size == id.size);  // at another base the old entries stay stale
            if (pass == 0) {
                g_id = id;
                g_mod = h;
                g_base = id.base;
                g_size = id.size;
                g_attachTick = GetTickCount64();
                g_retryStage = 0;
                h2::Msg att = NewMsg("attach");
                att.Add("base=%llx stamp=%x size=%x", static_cast<unsigned long long>(id.base), id.stamp, id.size);
                Send(att);
                printf("[h2] halo2.dll at %p (stamp %08x, size %x)\n", h, id.stamp, id.size);
            }
            if (notes[0]) printf("[h2] %s\n", notes);

            // 3. pin only around MinHook create+enable, re-check identity inside the pin, no printing while pinned
            int status[kHookCount];
            bool pinned = false, need = false;
            for (int i = 0; i < kHookCount; i++) {
                status[i] = kNotAttempted;
                if (!g_on[i] && g_addr[i]) need = true;
            }
            if (need) {
                Pin pin;
                ModId inPin;
                pinned = pin.Acquire(h) && ReadIdentity(h, &inPin) && inPin == id;
                if (pinned) {
                    g_hdrVar = d.hdrVar; g_gVar = d.gVar; g_actVar = d.actVar; g_tickVar = d.tickVar;  // commit before any jump exists
                    g_killRet = d.killRet; g_loadRet = d.loadRet; g_resetRet = d.resetRet;
                    g_claimRet = d.claimRet; g_skullArr = d.skullArr;
                    for (int i = 0; i < kHookCount; i++) {
                        if (g_on[i] || !g_addr[i]) continue;
                        void* target = const_cast<uint8_t*>(g_addr[i]);
                        g_target[i] = target;
                        status[i] = CreateAndEnable(i, target, sameBuild);
                        if (status[i] == MH_OK) g_on[i] = true;
                    }
                }
            } else {
                g_hdrVar = d.hdrVar; g_gVar = d.gVar; g_actVar = d.actVar; g_tickVar = d.tickVar;
                g_killRet = d.killRet; g_loadRet = d.loadRet; g_resetRet = d.resetRet;
                g_claimRet = d.claimRet; g_skullArr = d.skullArr;
                pinned = true;
            }
            if (!pinned) {
                printf("[h2] could not pin halo2.dll or it changed; will retry\n");
                return false;
            }

            // 4. report (unpinned)
            char on[200] = {}, off[240] = {};
            for (int i = 0; i < kHookCount; i++) {
                if (g_on[i]) {
                    AppendName(on, sizeof(on), kPatName[i]);
                    if (status[i] == MH_OK)
                        printf("[h2] hook %s installed at RVA %llx\n", kPatName[i], static_cast<unsigned long long>(g_addr[i] - base));
                    continue;
                }
                char entry[48];
                if (!g_addr[i]) snprintf(entry, sizeof(entry), "%s:%s", kPatName[i], g_hits[i] ? "ambig" : "none");
                else snprintf(entry, sizeof(entry), "%s:hookfail", kPatName[i]);
                AppendName(off, sizeof(off), entry);
                if (status[i] == kStaleBuild) printf("[h2] %s: stale MinHook entry from a different build, hook off\n", kPatName[i]);
                else if (status[i] != kNotAttempted) printf("[h2] %s: MinHook failed: %d (%s)\n", kPatName[i], status[i], MH_StatusToString(static_cast<MH_STATUS>(status[i])));
            }
            if (!g_killRet.load()) printf("[h2] unit_die -> actor_died link not verified: deaths are reported as kind=unknown\n");
            if (!g_loadRet.load()) printf("[h2] game_start_for_map -> game_start link not verified: starts are reported as kind=other\n");
            if (!g_tickVar.load()) printf("[h2] game_time_get not verified: events carry tick=-\n");
            if (!g_claimRet.load()) printf("[h2] skull_claim -> claim_notify link not verified: claim_notify lines are path=notify_unknown\n");
            if (!g_skullArr.load()) printf("[h2] skull active array not verified: skull_apply lines carry pre=- act=-\n");

            h2::Msg ins = NewMsg("install");
            ins.Add("pass=%d on=%s off=%s", pass, on[0] ? on : "-", off[0] ? off : "-");
            ins.Add("hdr=%d g=%d act=%d tick=%d kill=%d load=%d reset=%d", g_hdrVar.load() ? 1 : 0, g_gVar.load() ? 1 : 0,
                    g_actVar.load() ? 1 : 0, g_tickVar.load() ? 1 : 0, g_killRet.load() ? 1 : 0, g_loadRet.load() ? 1 : 0,
                    g_resetRet.load() ? 1 : 0);
            ins.Add("claim=%d skarr=%d", g_claimRet.load() ? 1 : 0, g_skullArr.load() ? 1 : 0);
            Send(ins);
            return true;
        }

        void Attach(HMODULE h) {
            if (!Pass(h, 0)) g_mod = nullptr;  // retried on the next 100 ms slice
        }

        void CheckIntegrity() {
            ModId id;
            const bool changed = !ReadIdentity(g_mod, &id) || !(id == g_id);
            bool intact[kHookCount];
            int enabled, intactCount;
            CheckPrologues(intact, &enabled, &intactCount);
            switch (h2::DecideReattach(changed, enabled, intactCount)) {
            case h2::Reattach::None:
                return;
            case h2::Reattach::DropChanged: {
                // Foreign change of some functions: keep the working hooks and every anchor, drop only the changed ones.
                char names[160] = {};
                for (int i = 0; i < kHookCount; i++) {
                    if (g_on[i] && !intact[i]) {
                        g_on[i] = false;
                        g_addr[i] = nullptr;  // a later retry rescans it (and finds nothing while the foreign patch is there)
                        AppendName(names, sizeof(names), kPatName[i]);
                    }
                }
                printf("[h2] prologue of %s changed by something else: those hooks dropped, the rest kept\n", names);
                h2::Msg m = NewMsg("hookdrop");
                m.Add("dropped=%s", names);
                Send(m);
                return;
            }
            case h2::Reattach::Full:
                break;
            }
            printf("[h2] halo2.dll identity changed or every hook prologue is gone: re-attaching\n");
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
            if (!g_mod) return;
        }
        // Bounded retries of whatever is still missing (the loader may still have been working at attach time)
        if (g_retryStage < 2) {
            ULONGLONG elapsed = GetTickCount64() - g_attachTick;
            if (elapsed >= (g_retryStage == 0 ? 2000ULL : 10000ULL)) {
                g_retryStage++;
                if (Incomplete()) Pass(g_mod, g_retryStage);
            }
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
