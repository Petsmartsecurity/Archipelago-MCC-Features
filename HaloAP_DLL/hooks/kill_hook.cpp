#include "kill_hook.h"
#include "../minhook/MinHook.h"
#include "../pattern_scan.h"
#include "shared/ai_event_format.h"
#include <cstdio>
#include <cstdint>
#include <cstring>

// Verified against halo1.dll build 2025.08.16.178512.1 (docs/research/h1-kill-path.md section 5).
// Each signature matches exactly once (gate: hook-patterns), and tools/h1/ce_symbols.py re-resolves the patterns and
// RIP loads below offline (gate: ce-symbols).
// What the signatures pin, so that an MCC update which moves them makes install fail instead of misreading:
//   actor size 0x66C, actor +0x28 (encounter) and +0x4C (variant)  -> kActorDiedPattern
//   scenario +0x42C, encounter size 0x58, enc +4/+6, squad size 0x20 (shl 5), squad +0x16/+0x18 -> kAiCountPattern
//   object header size 0xC / +8, swarm actor handle obj+0x1FC, actor +0x28/+0x2E -> kUnitDeathPattern
// NOT pinned (checked only by telemetry plausibility): swarm member count actor +0x14, actor +0x2E squad, +0x30 platoon, +0x06 flags, +0x10 unit,
// encounter +0x18/+0x28 living/total, object header/pool layout and obj+0xF6 kill flags.
namespace haloap {

    namespace {
        // actor_died(actor_handle): real deaths, plus erases reached through actor_erase. 0x69 bytes, to the pool load.
        const char* kActorDiedPattern =
            "48 89 5C 24 18 89 4C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 20 48 8B 05 ? ? ? ? 0F B7 D1 4C 69 "
            "C2 6C 06 00 00 4C 63 58 34 49 03 C0 4C 03 D8 41 8B 4B 4C E8 ? ? ? ? 45 8B 7B 28 45 33 ED F3 0F 10 1D ? "
            "? ? ? 41 83 CC FF 66 41 83 7B 52 03 48 8B D8 4C 8B 35 ? ? ? ? 0F 57 E4 48 8B 3D ? ? ? ?";
        // actor_erase(actor_handle, flag): calls actor_died, then deletes the unit (ai_erase, garbage collection).
        const char* kActorErasePattern =
            "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B 05 ? ? ? ? 40 8A EA 44 0F B7 C1 "
            "4D 69 C8 6C 06 00 00 48 63 78 34 49 03 C1 8B F1 48 03 F8 F6 47 06 01";
        // ai_index count (ai_living_count's implementation): only used to locate the scenario/encounter/squad globals.
        // 0xC7 bytes, through the squad record's +0x16 read.
        const char* kAiCountPattern =
            "48 89 5C ? ? 48 89 74 ? ? 48 89 7C ? ? 45 33 D2 44 8B D9 33 DB 49 8B F0 0F B7 FA 0F 57 C0 83 F9 FF 0F "
            "84 ? ? ? ? 48 8B 0D ? ? ? ? 41 8B C3 C1 E8 1E 85 C0 0F 84 ? ? ? ? 83 F8 01 41 0F B7 C3 0F 84 ? "
            "? ? ? 3B 81 2C 04 00 00 0F 8D ? ? ? ? 48 6B C8 58 48 8B 05 ? ? ? ? 41 C1 EB 10 48 63 50 34 48 03 "
            "C1 48 03 D0 B8 FF 00 00 00 66 44 23 D8 66 44 3B 5A 06 0F 8D ? ? ? ? 0F B7 42 04 66 41 03 C3 0F BF CF 48 "
            "0F BF D0 48 C1 E2 05 48 03 15 ? ? ? ? 85 C9 74 ? 83 F9 01 74 ? 0F BF 42 1A 0F BF 4A 18 2B C8 44 0F 49 "
            "D1 EB ? 44 0F BF 52 1A EB ? 44 0F BF 52 18 0F BF 5A 16";

        // unit_death(unit, feign, p3, p4) (0xb04564): the death handler of every unit; log-only swarm hook (PLAN 5.7). 314 bytes, from
        // the prologue through the swarm-actor branch (distinct from its near copy 0xb04990 by the prologue).
        // x64 ABI: rcx = unit handle, dl = feign flag (dl != 0 -> revivable "feign death", not a real death), r8/r9 = out pointers
        // the original only passes on; no stack arguments; returns void. Pins obj header size 0xC / +8, obj+0x1FC (swarm actor),
        // actor size 0x66C and the actor +0x28/+0x2E reads done right before swarm_member_removed (0xc044c8).
        const char* kUnitDeathPattern =
            "48 89 5C ? ? 48 89 6C ? ? 48 89 74 ? ? 57 41 54 41 55 41 56 41 57 48 83 EC 20 48 8B 35 ? ? ? ? 45 33 ED 48 "
            "8B 1D ? ? ? ? 41 83 CC FF 48 83 C3 34 44 0F B7 D1 4D 8B F1 8B F9 48 63 46 34 4D 8B F8 4B 8D 2C 52 48 C1 E5 "
            "02 48 03 C5 4C 63 54 30 08 49 03 DA 45 3B D4 49 0F 44 DD 84 D2 0F 85 ? ? ? ? B2 01 66 44 89 AB 04 04 00 00 "
            "8B CF E8 ? ? ? ? 8B 8B F8 01 00 00 41 3B CC 74 ? E8 ? ? ? ? 48 8B 35 ? ? ? ? 44 89 A3 F8 01 00 00 8B 8B C0 "
            "01 00 00 41 3B CC 74 ? 0F B7 C1 48 69 D0 6C 06 00 00 48 8B 05 ? ? ? ? 4C 63 40 34 48 03 C2 4C 03 C0 41 0F B7 "
            "40 28 66 89 83 18 03 00 00 41 0F B7 40 2E 66 89 83 1A 03 00 00 E8 ? ? ? ? 48 8B 35 ? ? ? ? 44 89 A3 C0 01 00 "
            "00 8B 8B C8 01 00 00 41 3B CC 74 ? 0F B7 C1 48 69 D0 6C 06 00 00 48 8B 05 ? ? ? ? 4C 63 40 34 48 03 C2 4C 03 "
            "C0 8B D7 41 0F B7 40 28 66 89 83 18 03 00 00 41 0F B7 40 2E 66 89 83 1A 03 00 00 E8 ? ? ? ? 48 8B 35 ? ? ? ? "
            "44 89 A3 C8 01 00 00";

        // RIP-relative loads inside the matched functions: (offset from function start, first 3 opcode bytes).
        struct RipLoad { size_t offset; uint8_t op[3]; };
        constexpr RipLoad kActorsFromDied     = { 0x18, { 0x48, 0x8B, 0x05 } };  // mov rax,[actors]
        constexpr RipLoad kObjHeadersFromDied = { 0x58, { 0x4C, 0x8B, 0x35 } };  // mov r14,[object headers]
        constexpr RipLoad kObjPoolFromDied    = { 0x62, { 0x48, 0x8B, 0x3D } };  // mov rdi,[object pool]
        constexpr RipLoad kObjHeadersFromUnit = { 0x1C, { 0x48, 0x8B, 0x35 } };  // mov rsi,[object headers] (== FromDied)
        constexpr RipLoad kObjPoolFromUnit    = { 0x26, { 0x48, 0x8B, 0x1D } };  // mov rbx,[object pool]    (== FromDied)
        constexpr RipLoad kActorsFromUnit     = { 0xFC, { 0x48, 0x8B, 0x05 } };  // mov rax,[actors]         (== FromDied)
        constexpr RipLoad kScenarioFromCnt    = { 0x29, { 0x48, 0x8B, 0x0D } };  // mov rcx,[scenario]
        constexpr RipLoad kEncountersFromCnt  = { 0x5B, { 0x48, 0x8B, 0x05 } };  // mov rax,[encounters]
        constexpr RipLoad kSquadsFromCnt      = { 0x97, { 0x48, 0x03, 0x15 } };  // add rdx,[squads]

        // Struct layout (V in the research doc; corrected by Phase 3 QA).
        constexpr int kDataArrayFirstElementOffset = 0x34;  // blam data array header: int offset to elements
        constexpr int kActorSize = 0x66C, kEncounterSize = 0x58, kSquadSize = 0x20, kObjHeaderSize = 0x0C;
        constexpr int kActorFlags = 0x06, kActorUnit = 0x10, kActorEncounter = 0x28, kActorSquad = 0x2E,
                      kActorPlatoon = 0x30, kActorVariant = 0x4C;
        constexpr int kEncFirstSquad = 0x04, kEncSquadCount = 0x06, kEncTotal = 0x18, kEncLiving = 0x28;
        constexpr int kSquadTotal = 0x16, kSquadLiving = 0x18;
        constexpr int kScenarioEncounterCount = 0x42C;
        constexpr int kObjSwarmActor = 0x1FC;     // obj+0x1FC: the unit's swarm actor handle (-1 = none)
        constexpr int kActorSwarmMembers = 0x14;  // swarm actor: member count (short)
        constexpr int kObjHeaderDataOffset = 0x08, kObjKillFlags = 0xF6;  // 0x20 unit_kill, 0x40 silent

        typedef void (*ActorDiedFn)(uint64_t actorHandle);
        typedef void (*ActorEraseFn)(uint64_t actorHandle, uint64_t flag);
        typedef void (*UnitDeathFn)(uint64_t unit, uint64_t feign, uint64_t p3, uint64_t p4);

        // Never cleared on uninstall: a game thread still inside a detour must always reach the original (QA m1).
        ActorDiedFn g_origDied = nullptr;
        ActorEraseFn g_origErase = nullptr;
        UnitDeathFn g_origUnitDeath = nullptr;
        void* g_unitDeathTarget = nullptr;
        void* g_diedTarget = nullptr;
        void* g_eraseTarget = nullptr;
        PipeClient* g_pipe = nullptr;

        uint8_t** g_actorsVar = nullptr;      // address of the global holding the actor data array pointer
        uint8_t** g_objHeadersVar = nullptr;
        uint8_t** g_objPoolVar = nullptr;
        uint8_t** g_scenarioVar = nullptr;
        uint8_t** g_encountersVar = nullptr;
        uint8_t** g_squadsVar = nullptr;      // global holding the flat squad array base

        thread_local int t_eraseDepth = 0;    // >0 while inside actor_erase on this thread (depth, not bool: QA)

        struct ActorInfo {
            uint32_t ah = 0xFFFFFFFF, unit = 0xFFFFFFFF;
            int enc = -1, sq = -1, pl = -1;
            uint32_t variant = 0xFFFFFFFF;
            uint32_t flags = 0;
            uint32_t killFlags = 0xFF;  // 0xFF = unreadable
            bool ok = false;
        };

        uint8_t** ResolveRip(uint8_t* fn, const RipLoad& load) {
            uint8_t* ins = fn + load.offset;
            if (memcmp(ins, load.op, 3) != 0) return nullptr;  // fail closed if the instruction moved
            int32_t disp;
            memcpy(&disp, ins + 3, sizeof(disp));
            return reinterpret_cast<uint8_t**>(ins + 7 + disp);
        }

        bool Plausible(const void* p) {
            auto v = reinterpret_cast<uintptr_t>(p);
            return v >= 0x10000 && v < 0x7FFFFFFFFFFF;
        }

        // Plain C helpers: no C++ objects with destructors, so __try is allowed.
        uint8_t* DataArrayElement(uint8_t* array, uint32_t handle, int size) {
            if (!Plausible(array)) return nullptr;
            int32_t first = *reinterpret_cast<int32_t*>(array + kDataArrayFirstElementOffset);
            return array + first + static_cast<size_t>(handle & 0xFFFF) * size;
        }

        // The object's data through the object header table, exactly as actor_kill (0xc03f58) does. Null if none.
        // Callers must be inside __try.
        uint8_t* ObjectData(uint32_t unitHandle) {
            if (unitHandle == 0xFFFFFFFF) return nullptr;
            uint8_t* headers = *g_objHeadersVar;
            uint8_t* pool = *g_objPoolVar;
            if (!Plausible(headers) || !Plausible(pool)) return nullptr;
            int32_t first = *reinterpret_cast<int32_t*>(headers + kDataArrayFirstElementOffset);
            int32_t dataOffset = *reinterpret_cast<int32_t*>(
                headers + first + kObjHeaderDataOffset + static_cast<size_t>(unitHandle & 0xFFFF) * kObjHeaderSize);
            if (dataOffset == -1) return nullptr;
            return pool + dataOffset;
        }

        // The unit's kill flags (0xFF = unreadable).
        uint32_t UnitKillFlags(uint32_t unitHandle) {
            uint8_t* obj = ObjectData(unitHandle);
            if (!obj) return 0xFF;
            return *reinterpret_cast<uint16_t*>(obj + kObjKillFlags) & 0x60;
        }

        void ReadActor(uint32_t actorHandle, ActorInfo* out) {
            __try {
                uint8_t* actor = DataArrayElement(*g_actorsVar, actorHandle, kActorSize);
                if (!actor) return;
                out->ah = actorHandle;
                uint32_t encHandle = *reinterpret_cast<uint32_t*>(actor + kActorEncounter);
                out->enc = encHandle == 0xFFFFFFFF ? -1 : static_cast<int>(encHandle & 0xFFFF);
                out->sq = *reinterpret_cast<int16_t*>(actor + kActorSquad);
                out->pl = *reinterpret_cast<int16_t*>(actor + kActorPlatoon);
                out->variant = *reinterpret_cast<uint32_t*>(actor + kActorVariant);
                out->flags = *(actor + kActorFlags);
                out->unit = *reinterpret_cast<uint32_t*>(actor + kActorUnit);
                out->ok = true;
                out->killFlags = UnitKillFlags(out->unit);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                // ok stays as set: actor fields read before a fault in UnitKillFlags are still valid
            }
        }

        // A Flood infection form (swarm member) is about to die: the swarm actor's fields, read BEFORE the original removes it.
        struct SwarmInfo {
            uint32_t ah = 0xFFFFFFFF, unit = 0xFFFFFFFF, variant = 0xFFFFFFFF;
            int enc = -1, sq = -1, members = -1;
            uint32_t killFlags = 0xFF;
        };

        // True if `unit` has a swarm actor (obj+0x1FC != -1). Reads only; SEH around every read.
        bool ReadSwarm(uint32_t unit, SwarmInfo* out) {
            __try {
                uint8_t* obj = ObjectData(unit);
                if (!obj) return false;
                uint32_t actorHandle = *reinterpret_cast<uint32_t*>(obj + kObjSwarmActor);
                if (actorHandle == 0xFFFFFFFF) return false;
                out->unit = unit;
                out->ah = actorHandle;
                out->killFlags = *reinterpret_cast<uint16_t*>(obj + kObjKillFlags) & 0x60;
                uint8_t* actor = DataArrayElement(*g_actorsVar, actorHandle, kActorSize);
                if (!actor) return true;  // still a swarm death; the actor fields stay -1
                uint32_t encHandle = *reinterpret_cast<uint32_t*>(actor + kActorEncounter);
                out->enc = encHandle == 0xFFFFFFFF ? -1 : static_cast<int>(encHandle & 0xFFFF);
                out->sq = *reinterpret_cast<int16_t*>(actor + kActorSquad);
                out->variant = *reinterpret_cast<uint32_t*>(actor + kActorVariant);
                out->members = *reinterpret_cast<int16_t*>(actor + kActorSwarmMembers);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                return out->ah != 0xFFFFFFFF;  // fields read before a fault are kept
            }
        }

        // After the original ran (encounter recount done): squad record and encounter counters.
        void ReadCounts(int enc, int sq, int* living, int* total, int* encLiving, int* encTotal) {
            *living = *total = *encLiving = *encTotal = -1;
            if (enc < 0) return;
            __try {
                uint8_t* scenario = *g_scenarioVar;
                if (!Plausible(scenario)) return;
                if (enc >= *reinterpret_cast<int32_t*>(scenario + kScenarioEncounterCount)) return;
                uint8_t* e = DataArrayElement(*g_encountersVar, static_cast<uint32_t>(enc), kEncounterSize);
                if (!e) return;
                *encLiving = *reinterpret_cast<int16_t*>(e + kEncLiving);
                *encTotal = *reinterpret_cast<int16_t*>(e + kEncTotal);
                uint8_t* squads = *g_squadsVar;
                int16_t first = *reinterpret_cast<int16_t*>(e + kEncFirstSquad);
                int16_t count = *reinterpret_cast<int16_t*>(e + kEncSquadCount);
                if (sq < 0 || sq >= count || first < 0 || !Plausible(squads)) return;
                uint8_t* rec = squads + static_cast<size_t>(first + sq) * kSquadSize;
                *total = *reinterpret_cast<int16_t*>(rec + kSquadTotal);
                *living = *reinterpret_cast<int16_t*>(rec + kSquadLiving);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                *living = *total = -1;
            }
        }

        void Report(bool kill, const ActorInfo& a, int living, int total, int encLiving, int encTotal) {
            PipeClient* pipe = g_pipe;
            if (!pipe || !pipe->IsConnected()) return;
            char buf[256];
            snprintf(buf, sizeof(buf), kAiEventFormat, kill ? "KILL" : "ERASE", a.ah, a.unit, a.enc, a.sq, a.pl,
                     a.variant, a.flags & 0xFF, a.killFlags & 0xFF, living, total, encLiving, encTotal,
                     GetCurrentThreadId());
            pipe->SendAsync(buf);
        }

        void DetourActorDied(uint64_t actorHandle) {
            ActorInfo info;
            ReadActor(static_cast<uint32_t>(actorHandle), &info);  // before: the original detaches the actor
            g_origDied(actorHandle);
            if (!info.ok) return;
            int living, total, encLiving, encTotal;
            ReadCounts(info.enc, info.sq, &living, &total, &encLiving, &encTotal);
            Report(t_eraseDepth == 0, info, living, total, encLiving, encTotal);
        }

        void ReportSwarm(const SwarmInfo& w) {
            PipeClient* pipe = g_pipe;
            if (!pipe || !pipe->IsConnected()) return;
            char buf[192];
            snprintf(buf, sizeof(buf), kSwarmEventFormat, w.ah, w.unit, w.enc, w.sq, w.variant, w.killFlags & 0xFF,
                     w.members, GetCurrentThreadId());
            pipe->SendAsync(buf);
        }

        // unit_death(unit, feign, p3, p4). Log-only: reads, calls the original with the untouched arguments, sends one line.
        // A feign (dl != 0) is not a death; a unit with no swarm actor is not a swarm member; inside actor_erase it is a removal.
        void DetourUnitDeath(uint64_t unit, uint64_t feign, uint64_t p3, uint64_t p4) {
            SwarmInfo w;
            bool swarm = (feign & 0xFF) == 0 && t_eraseDepth == 0 && ReadSwarm(static_cast<uint32_t>(unit), &w);
            g_origUnitDeath(unit, feign, p3, p4);
            if (swarm) ReportSwarm(w);
        }

        void DetourActorErase(uint64_t actorHandle, uint64_t flag) {
            ++t_eraseDepth;
            g_origErase(actorHandle, flag);
            --t_eraseDepth;
        }

        bool Hook(void* target, void* detour, void** original, const char* name) {
            MH_STATUS s = MH_CreateHook(target, detour, original);
            if (s != MH_OK) {
                printf("[hook] (kill) MH_CreateHook %s failed: %d (%s)\n", name, s, MH_StatusToString(s));
                return false;
            }
            s = MH_EnableHook(target);
            if (s != MH_OK) {
                printf("[hook] (kill) MH_EnableHook %s failed: %d (%s)\n", name, s, MH_StatusToString(s));
                MH_RemoveHook(target);
                return false;
            }
            return true;
        }
    }

    namespace {
        // Fail closed: if anything about unit_death does not match (signatures, or its globals differing from the ones
        // actor_died resolved), the swarm hook is simply not installed. Returns true if installed.
        bool InstallSwarmHook(HMODULE halo1) {
            auto unit = static_cast<uint8_t*>(FindPatternInModule(halo1, kUnitDeathPattern));
            if (!unit) {
                printf("[hook] (swarm) not installed: unit_death pattern not found\n");
                return false;
            }
            if (ResolveRip(unit, kObjHeadersFromUnit) != g_objHeadersVar || ResolveRip(unit, kObjPoolFromUnit) != g_objPoolVar ||
                ResolveRip(unit, kActorsFromUnit) != g_actorsVar) {
                printf("[hook] (swarm) not installed: globals differ from actor_died's\n");
                return false;
            }
            if (!Hook(unit, (void*)DetourUnitDeath, (void**)&g_origUnitDeath, "unit_death")) return false;
            g_unitDeathTarget = unit;
            printf("[hook] (swarm) installed: unit_death=%p (log-only)\n", unit);
            return true;
        }
    }

    bool InstallKillHook(PipeClient* pipe) {
        g_pipe = pipe;
        HMODULE halo1 = GetModuleHandleA("halo1.dll");
        if (!halo1) {
            printf("[hook] (kill) halo1.dll not loaded yet\n");
            return false;
        }
        auto died = static_cast<uint8_t*>(FindPatternInModule(halo1, kActorDiedPattern));
        auto erase = static_cast<uint8_t*>(FindPatternInModule(halo1, kActorErasePattern));
        auto count = static_cast<uint8_t*>(FindPatternInModule(halo1, kAiCountPattern));
        if (!died || !erase || !count) {
            printf("[hook] (kill) pattern not found: died=%p erase=%p count=%p\n", died, erase, count);
            return false;
        }
        g_actorsVar = ResolveRip(died, kActorsFromDied);
        g_objHeadersVar = ResolveRip(died, kObjHeadersFromDied);
        g_objPoolVar = ResolveRip(died, kObjPoolFromDied);
        g_scenarioVar = ResolveRip(count, kScenarioFromCnt);
        g_encountersVar = ResolveRip(count, kEncountersFromCnt);
        g_squadsVar = ResolveRip(count, kSquadsFromCnt);
        if (!g_actorsVar || !g_objHeadersVar || !g_objPoolVar || !g_scenarioVar || !g_encountersVar || !g_squadsVar) {
            printf("[hook] (kill) global resolution failed (instruction layout changed)\n");
            return false;
        }
        // Install erase first so no death is ever reported without its erase guard in place.
        if (!Hook(erase, (void*)DetourActorErase, (void**)&g_origErase, "actor_erase")) return false;
        g_eraseTarget = erase;
        if (!Hook(died, (void*)DetourActorDied, (void**)&g_origDied, "actor_died")) {
            UninstallKillHook();
            return false;
        }
        g_diedTarget = died;
        printf("[hook] (kill) installed: actor_died=%p actor_erase=%p\n", died, erase);
        InstallSwarmHook(halo1);  // optional and log-only: any failure leaves the kill hook as it is
        return true;
    }

    void UninstallKillHook() {
        if (g_unitDeathTarget) {  // first: stop reporting swarm deaths
            MH_DisableHook(g_unitDeathTarget);
            MH_RemoveHook(g_unitDeathTarget);
            g_unitDeathTarget = nullptr;
        }
        // Remove actor_died first, so no death is reported after the erase guard is gone.
        if (g_diedTarget) {
            MH_DisableHook(g_diedTarget);
            MH_RemoveHook(g_diedTarget);
            g_diedTarget = nullptr;
        }
        if (g_eraseTarget) {
            MH_DisableHook(g_eraseTarget);
            MH_RemoveHook(g_eraseTarget);
            g_eraseTarget = nullptr;
        }
        g_pipe = nullptr;  // Report() copies the pointer once, so a detour in flight never sees it change mid-use
    }

}  // namespace haloap
