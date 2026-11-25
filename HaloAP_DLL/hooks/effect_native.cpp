#include "effect_native.h"
#include "../minhook/MinHook.h"
#include "../pattern_scan.h"
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>

// Native effect ops for Halo CE (ADR-005 phase B). Research: docs/research/item-effects-phase-b.md; QA conditions
// docs/stage-reports/item-effects-b-research-qa.md (M1..M4, m1..m8). Object frames: "P" = the pointer 0xb389a4 returns
// (pool + 0x34 + offset); every field offset below is P-relative, as the research tables give them.
//
// Rules this file follows:
//  - every address is a unique pattern (FindUniquePatternInModule) or derived from one (RIP-relative global, call target);
//  - nothing is written into tag data, only into object memory (unit / weapon), and only after the handle was validated;
//  - everything runs under SEH; an exception fails the op, never the game;
//  - the gateway always pops the HS stack (0xacbfac) after the op, whatever happened (QA M2).

namespace haloap {
    namespace {
        using fx::Op;
        using fx::Result;

        // ---- signatures (halo1.dll build 2025.08.16.178512.1; the effect-natives gate checks each is unique, and pins the RVAs) --
        const char* kPatSpawnWarthog =  // the cheat the gateway replaces (called by its evaluate): only used to verify the table entry
            "48 89 5C ? ? 48 89 74 ? ? 48 89 7C ? ? 41 56 48 83 EC 20 48 8B 05 ? ? ? ? 33 DB 48 8B 15 ? ? ? ? 4C 8B 05 ? ? ? ?";
        const char* kPatPopStack =  // 0xacbfac: end of an HS evaluate (pop the thread's stack frame); the evaluate tail-jumps to it
            "48 89 5C ? ? 48 89 6C ? ? 48 89 74 ? ? 57 48 83 EC 20 4C 8B 0D ? ? ? ? 8B EA 0F B7 C9 48 69 F1 14 05 00 00 48 8B 0D ? ? ? ?";
        const char* kPatPlayerHandle =  // 0xb21e34: first player's handle; its first load is the player datum array
            "48 89 5C ? ? 57 48 83 EC 30 48 8B 05 ? ? ? ? 83 CB FF 33 FF 48 89 44 ? ? 66 89 7C ? ? 89 5C ? ? EB ? 39 58 64";
        const char* kPatDatum =  // 0xbbb8d0: datum lookup (array, handle) -> pointer or null
            "45 33 C0 4C 8B D1 83 FA FF 74 ? 44 8B CA 41 C1 F9 10 66 85 D2 78 ? 66 3B 51 20 7D ? 0F B7 49 22 0F BF C2 0F AF C1 49 63 4A 34";
        const char* kPatObject =  // 0xb389a4: (handle, type mask) -> object pointer or null
            "40 53 48 83 EC 20 8B DA 8B D1 48 8B 0D ? ? ? ? E8 ? ? ? ? 4C 8B D8 33 C0 4D 85 DB 74 ? 41 0F B6 4B 03 0F A3 CB";
        const char* kPatPlaceInit =  // 0xb35ebc: object placement buffer init (buffer, tag id, owner)
            "48 89 5C ? ? 48 89 74 ? ? 57 48 83 EC 20 41 8B F0 8B DA 48 8B F9 33 D2 48 83 C1 08 41 B8 84 00 00 00 E8 ? ? ? ?";
        const char* kPatPlaceCreate =  // 0xb35f80: create the object from the buffer -> handle
            "48 83 EC 28 8A 05 ? ? ? ? 4C 8B C9 F6 D8 45 1B C0 41 83 E0 04 41 FF C8 66 83 3D ? ? ? ? 02 75 ? 8B 09 E8 ? ? ? ?";
        const char* kPatObjPos =  // 0xb37de0: object world position
            "48 89 5C ? ? 48 89 74 ? ? 57 48 83 EC 20 48 8B 35 ? ? ? ? 4C 8B D2 0F B7 C9 4C 8D 5E 4C 4C 8D 04 49 48 8B 0D ? ? ? ?";
        const char* kPatObjFwd =  // 0xb37f98: object forward / up vectors
            "48 89 5C ? ? 48 89 74 ? ? 57 48 83 EC 20 48 8B 35 ? ? ? ? 4D 8B D0 0F B7 C9 48 8B DA 4C 8D 0C 49 48 8B 0D ? ? ? ?";
        const char* kPatTagData =  // 0xa9b648: tag data pointer (tag index)
            "48 8B 05 ? ? ? ? 48 0F BF D1 48 C1 E2 05 48 63 4C 02 14 33 C0 85 C9 74 ? 48 8B C1 48 2B 05 ? ? ? ? 48 03 05 ? ? ? ?";
        const char* kPatTagName =  // 0xa9b25c: tag name (tag index)
            "48 8B 05 ? ? ? ? 48 0F BF D1 48 C1 E2 05 48 63 4C 02 10 33 C0 85 C9 74 ? 48 8B C1 48 2B 05 ? ? ? ? 48 03 05 ? ? ? ?";
        const char* kPatTagIter =  // 0xa9b43c: tag iterator step
            "48 8B 05 ? ? ? ? 83 CA FF 44 0F BF 41 08 4C 8B C9 44 8B 58 0C 45 3B C3 7D ? 49 0F BF C0 66 41 FF C0 48 C1 E0 05 48 03 05 ? ? ? ?";
        const char* kPatOvershield =  // 0xb9dc40: start an overshield on a unit (refuses above 1.0)
            "48 89 5C ? ? 48 89 74 ? ? 57 48 83 EC 20 4C 8B 15 ? ? ? ? 45 33 C0 F3 0F 10 05 ? ? ? ? 83 CE FF 0F B7 D1 8B F9";
        const char* kPatShieldDepleted =  // 0xb9db90: the engine's own "shield depleted" hook (sound / flash)
            "48 89 5C ? ? 48 89 74 ? ? 57 48 83 EC 20 48 8B 1D ? ? ? ? 33 F6 48 83 C3 34 0F B7 D1 8B F9 4C 8D 04 52 48 8B 15 ? ? ? ?";
        const char* kPatCamo =  // 0xad66c4: start active camo / full spectrum (player handle, kind, ticks)
            "48 8B C4 48 89 ? ? 48 89 ? ? 48 89 ? ? 48 89 ? ? 41 56 48 83 EC 20 8B F1 0F B7 DA 8B D1 41 0F B7 E8 48 8B 0D ? ? ? ?";
        const char* kPatSound =  // 0xb32f00: sound_impulse_start's body (tag id, object or -1, gain)
            "83 F9 FF 0F 84 ? ? ? ? 48 8B C4 48 89 ? ? 48 89 ? ? 48 89 ? ? 4C 89 ? ? 55 48 8D ? A1 48 81 EC F0 00 00 00";
        const char* kPatSetAmmo =  // 0xb76ce4: set loaded / reserve rounds, clamped to the weapon tag's maxima
            "40 53 48 83 EC 20 4C 8B 1D ? ? ? ? 33 DB 0F B7 C9 49 83 C3 34 48 8D 14 49 48 8B 0D ? ? ? ? 48 63 41 34 48 03 C8";
        const char* kPatBattery =  // 0xb7b44c: weapon charge fraction (energy battery, or loaded rounds)
            "48 89 5C ? ? 48 89 6C ? ? 48 89 74 ? ? 57 48 83 EC 20 48 8B 3D ? ? ? ? 33 DB 0F B7 D1 48 83 C7 34 8B E9 0F 28 D1";
        const char* kPatWeaponMark =  // 0xc0a50c: mark the weapon's rounds as changed (replication / HUD)
            "48 89 5C ? ? 48 89 74 ? ? 57 48 83 EC 20 8B F1 E8 ? ? ? ? 33 DB 84 C0 0F 84 ? ? ? ? 48 8B 15 ? ? ? ? 8A 42 11";
        const char* kPatCinematic =  // 0xac62b0: cinematic_start; sets byte [*global + 10] = 1 (the cutscene flag)
            "40 53 48 83 EC 30 E8 ? ? ? ? 48 8D 0D ? ? ? ? E8 ? ? ? ? E8 ? ? ? ? 48 8B 05 ? ? ? ? 48 8B 15 ? ? ? ?";
        // instruction offsets inside those functions (each verified against its opcode bytes at install)
        constexpr int kPlayersLoadOff = 10;     // kPatPlayerHandle: mov rax,[rip+players datum array]
        constexpr int kCinematicLoadOff = 0x50; // kPatCinematic: mov rax,[rip+cinematic state]; mov byte [rax+0xA],1

        // ---- engine function types (x64 fastcall) --------------------------------------------------------------------------
        typedef uint32_t (*PlayerHandleFn)();
        typedef void* (*DatumFn)(void* array, int32_t handle);
        typedef uint8_t* (*ObjectFn)(uint32_t handle, uint32_t typeMask);
        typedef void (*PlaceInitFn)(void* buf, uint32_t tagId, int32_t owner);
        typedef uint32_t (*PlaceCreateFn)(void* buf);
        typedef void (*ObjPosFn)(uint32_t handle, float* out3);
        typedef void (*ObjFwdFn)(uint32_t handle, float* fwd3, float* up3);
        typedef uint8_t* (*TagDataFn)(int index);
        typedef const char* (*TagNameFn)(int index);
        typedef int32_t (*TagIterFn)(void* iter);
        typedef bool (*OvershieldFn)(uint32_t unit);
        typedef void (*UnitFn)(uint32_t unit);
        typedef bool (*CamoFn)(uint32_t player, uint32_t kind, int32_t ticks);
        typedef void (*SoundFn)(int32_t tag, int32_t object, float gain);
        typedef void (*SetAmmoFn)(uint32_t weapon, uint64_t unused, int16_t loaded, int16_t reserve);
        typedef void (*BatteryFn)(uint32_t weapon, float fraction);
        typedef void (*PopStackFn)(uint32_t thread, uint32_t zero);

        struct Natives {
            PlayerHandleFn playerHandle = nullptr;
            DatumFn datum = nullptr;
            ObjectFn object = nullptr;
            PlaceInitFn placeInit = nullptr;
            PlaceCreateFn placeCreate = nullptr;
            ObjPosFn objPos = nullptr;
            ObjFwdFn objFwd = nullptr;
            TagDataFn tagData = nullptr;
            TagNameFn tagName = nullptr;
            TagIterFn tagIter = nullptr;
            OvershieldFn overshield = nullptr;
            UnitFn shieldDepleted = nullptr;
            CamoFn camo = nullptr;
            SoundFn sound = nullptr;
            SetAmmoFn setAmmo = nullptr;
            BatteryFn battery = nullptr;
            UnitFn weaponMark = nullptr;
            PopStackFn popStack = nullptr;
            void** playersVar = nullptr;   // global holding the player datum array pointer
            uint8_t** cinematicVar = nullptr;  // global holding the pointer to the cinematic state block
        };
        Natives N;
        std::atomic<uint32_t> g_available{ 0 };  // bit per fx::Op
        std::atomic<bool> g_hooked{ false };
        void* g_gatewayTarget = nullptr;
        fx::OpSlot g_slot;
        char g_lastDrop[128] = "";
        int g_threadLogs = 0;

        // P-frame offsets (research table). Not game-version dependent in any way we can detect: a rebuilt halo1.dll fails the
        // pattern gate first.
        constexpr int kObjType = 0x70;      // i16, 0 = biped
        constexpr int kObjShield = 0xA0;    // f32
        constexpr int kObjStun = 0xC0;      // i16, shield recharge delay in ticks
        constexpr int kObjFlags = 0xC2;     // u16: 0x04 dead, 0x10 overshield active, 0x1000 recharging
        constexpr int kObjParent = 0xD8;    // i32, -1 when not attached to anything (vehicle seat)
        constexpr int kUnitSlot = 0x2D5;    // u8, current weapon slot, 0xFF none
        constexpr int kUnitWeapons = 0x2D8; // u32[4]
        constexpr int kUnitGrenades = 0x2FC;// u8[4]
        constexpr int kPlayerUnit = 0x64;   // player datum: unit handle
        constexpr int kWpReserve = 0x286, kWpLoaded = 0x28A, kWpReserve2 = 0x288, kWpLoaded2 = 0x28C;
        constexpr int kTagMagazines = 0x4F0;  // weapon tag: magazine reflexive count
        constexpr float kDropDistance = 1.5f, kDropHeight = 0.8f;  // the engine cheat's own constants

        bool IsExecutable(HMODULE h, const void* p) {
            auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(h);
            auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(h) + dos->e_lfanew);
            auto sec = IMAGE_FIRST_SECTION(nt);
            const uint8_t* b = static_cast<const uint8_t*>(p);
            for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++) {
                const uint8_t* s = reinterpret_cast<const uint8_t*>(h) + sec[i].VirtualAddress;
                if ((sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) && b >= s && b < s + sec[i].Misc.VirtualSize) return true;
            }
            return false;
        }
        bool InImage(HMODULE h, const void* p) {
            auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(h);
            auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(h) + dos->e_lfanew);
            const uint8_t* b = static_cast<const uint8_t*>(p);
            return b >= reinterpret_cast<const uint8_t*>(h) && b < reinterpret_cast<const uint8_t*>(h) + nt->OptionalHeader.SizeOfImage;
        }

        // 48 8B 05 disp32 at fn+off -> address of the global
        void** ResolveRipLoad(HMODULE h, const uint8_t* fn, int off) {
            if (!fn || fn[off] != 0x48 || fn[off + 1] != 0x8B || fn[off + 2] != 0x05) return nullptr;
            int32_t disp;
            std::memcpy(&disp, fn + off + 3, 4);
            void** v = reinterpret_cast<void**>(const_cast<uint8_t*>(fn) + off + 7 + disp);
            return InImage(h, v) ? v : nullptr;
        }

        const uint8_t* Find(HMODULE h, const char* pat, const char* name) {
            int hits = 0;
            const uint8_t* p = static_cast<const uint8_t*>(FindUniquePatternInModule(h, pat, &hits));
            if (!p) printf("[effects-native] signature %s: %s\n", name, hits ? "ambiguous (2+ matches)" : "not found");
            return p;
        }

        // Entry of `cheat_spawn_warthog` in the HS function table: [ret u16 = 4 ...][name*][parser*][evaluate*] (research M2).
        const uint8_t* ResolveGatewayEvaluate(HMODULE h) {
            auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(h);
            auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(h) + dos->e_lfanew);
            auto sec = IMAGE_FIRST_SECTION(nt);
            static const char kName[] = "\0cheat_spawn_warthog";  // leading NUL: a whole identifier, not a suffix
            const uint8_t* nameAddr = nullptr;
            int nameHits = 0;
            for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++) {
                if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) continue;
                const uint8_t* s = reinterpret_cast<const uint8_t*>(h) + sec[i].VirtualAddress;
                size_t len = sec[i].Misc.VirtualSize;
                for (size_t o = 0; o + sizeof(kName) <= len; o++) {
                    if (s[o] == 0 && std::memcmp(s + o, kName, sizeof(kName)) == 0 ) { nameAddr = s + o + 1; nameHits++; }
                }
            }
            if (nameHits != 1) { printf("[effects-native] gateway: name string hits=%d\n", nameHits); return nullptr; }
            const uint64_t* entryName = nullptr;
            int refHits = 0;
            for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++) {
                if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) continue;
                const uint8_t* s = reinterpret_cast<const uint8_t*>(h) + sec[i].VirtualAddress;
                size_t len = sec[i].Misc.VirtualSize;
                for (size_t o = 0; o + 24 <= len; o += 8) {
                    const uint64_t* q = reinterpret_cast<const uint64_t*>(s + o);
                    if (q[0] == reinterpret_cast<uint64_t>(nameAddr) && IsExecutable(h, reinterpret_cast<const void*>(q[1])) &&
                        IsExecutable(h, reinterpret_cast<const void*>(q[2]))) { entryName = q; refHits++; }
                }
            }
            if (refHits != 1) { printf("[effects-native] gateway: table entry hits=%d\n", refHits); return nullptr; }
            const uint16_t* head = reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(entryName) - 8);
            const uint16_t argc = *reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(entryName) - 8 + 0x30);
            if (head[0] != 4 || argc != 0) { printf("[effects-native] gateway: entry is not a void 0-argument function\n"); return nullptr; }
            return reinterpret_cast<const uint8_t*>(entryName[2]);
        }

        // 40 53 | 48 83 EC 20 | 8B DA | E8 rel32 (0xb21a54) | 33 D2 | 8B CB | 48 83 C4 20 | 5B | E9 rel32 (0xacbfac)
        bool VerifyGatewayBody(const uint8_t* ev, const uint8_t* warthogSpawn, const uint8_t* popStack) {
            static const uint8_t kHead[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0xDA, 0xE8 };
            static const uint8_t kMid[] = { 0x33, 0xD2, 0x8B, 0xCB, 0x48, 0x83, 0xC4, 0x20, 0x5B, 0xE9 };
            if (std::memcmp(ev, kHead, sizeof(kHead)) != 0 || std::memcmp(ev + 13, kMid, sizeof(kMid)) != 0) return false;
            int32_t r1, r2;
            std::memcpy(&r1, ev + 9, 4);
            std::memcpy(&r2, ev + 23, 4);
            return ev + 13 + r1 == warthogSpawn && ev + 27 + r2 == popStack;
        }

        // ---- ops ---------------------------------------------------------------------------------------------------------
        struct Ctx {
            uint32_t player = 0xFFFFFFFFu;
            uint32_t unit = 0xFFFFFFFFu;
            uint8_t* u = nullptr;
        };

        // M3: the player, its unit and its object class are checked before any native sees them. Returns null when fine.
        const char* Validate(Ctx& c, bool onFoot) {
            uint8_t* cin = N.cinematicVar ? *N.cinematicVar : nullptr;
            if (!cin) return "cutscene state not readable";
            if (cin[10]) return "cutscene";
            void* arr = N.playersVar ? *N.playersVar : nullptr;
            if (!arr) return "no player table";
            c.player = N.playerHandle();
            if (c.player == 0xFFFFFFFFu) return "no player";
            uint8_t* d = static_cast<uint8_t*>(N.datum(arr, static_cast<int32_t>(c.player)));
            if (!d) return "no player datum";
            c.unit = *reinterpret_cast<uint32_t*>(d + kPlayerUnit);
            if (c.unit == 0xFFFFFFFFu) return "no unit (dead or not spawned)";
            c.u = N.object(c.unit, 3);  // mask 3 = unit classes
            if (!c.u) return "unit not found";
            if (*reinterpret_cast<int16_t*>(c.u + kObjType) != 0) return "player unit is not a biped";
            if (*reinterpret_cast<uint16_t*>(c.u + kObjFlags) & 0x0004) return "dead";
            if (onFoot && *reinterpret_cast<int32_t*>(c.u + kObjParent) != -1) return "in a vehicle";
            return nullptr;
        }

        // Tag ids of the named tags of one class (exact names). found[i] = id of names[i], or -1.
        void FindTags(uint32_t cls, const char* const* names, size_t n, int32_t* found) {
            for (size_t i = 0; i < n; i++) found[i] = -1;
            alignas(8) uint8_t it[0x20];
            std::memset(it, 0, sizeof(it));
            *reinterpret_cast<int32_t*>(it + 0xC) = -1;
            *reinterpret_cast<uint32_t*>(it + 0x10) = cls;
            for (int guard = 0; guard < 100000; guard++) {
                int32_t id = N.tagIter(it);
                if (id == -1) break;
                const char* nm = N.tagName(static_cast<int16_t>(id));
                if (!nm) continue;
                for (size_t i = 0; i < n; i++) if (found[i] == -1 && std::strcmp(nm, names[i]) == 0) found[i] = id;
            }
        }

        // M1 (ammo): full reserve and loaded rounds from the tag maxima (0xb76ce4 clamps), both pairs, then the engine's mark.
        bool FillWeapon(uint32_t wh) {
            uint8_t* w = N.object(wh, 4);  // mask 4 = weapon
            if (!w) return false;
            const uint32_t tid = *reinterpret_cast<uint32_t*>(w);
            if (tid == 0xFFFFFFFFu) return false;
            uint8_t* td = N.tagData(static_cast<int16_t>(tid));
            if (!td) return false;
            const int32_t mags = *reinterpret_cast<int32_t*>(td + kTagMagazines);
            N.battery(wh, 1.0f);  // energy weapons: battery full (H, R in game); others: loaded rounds
            if (mags > 0 && mags <= 8) {  // M3: a count of 0 would make 0xb76ce4 read through a null tag pointer
                N.setAmmo(wh, 0, 0x7FFF, 0x7FFF);
                const int16_t reserve = *reinterpret_cast<int16_t*>(w + kWpReserve2);
                const int16_t loaded = *reinterpret_cast<int16_t*>(w + kWpLoaded2);
                if (loaded > 0) {  // plasma weapons have all-zero magazines: nothing to copy
                    *reinterpret_cast<int16_t*>(w + kWpReserve) = reserve;
                    *reinterpret_cast<int16_t*>(w + kWpLoaded) = loaded;
                }
            }
            N.weaponMark(wh);
            return true;
        }

        Result OpAmmoRefill(Ctx& c, const char** why) {
            const uint8_t slot = *(c.u + kUnitSlot);
            if (slot > 3) { *why = "no held weapon"; return Result::Deferred; }
            const uint32_t wh = *reinterpret_cast<uint32_t*>(c.u + kUnitWeapons + 4 * slot);
            if (wh == 0xFFFFFFFFu) { *why = "no held weapon"; return Result::Deferred; }
            if (!FillWeapon(wh)) { *why = "held weapon not valid"; return Result::Failed; }
            return Result::Applied;
        }

        Result OpGrenadePack(Ctx& c, const char**) {
            for (int i = 0; i < 2; i++) {  // frag and plasma
                uint8_t* g = c.u + kUnitGrenades + i;
                *g = static_cast<uint8_t>(fx::GrenadesAfterPack(*g));
            }
            return Result::Applied;
        }

        Result OpOvershield(Ctx& c, const char**) {
            if (!N.overshield(c.unit)) *reinterpret_cast<float*>(c.u + kObjShield) = fx::kOvershieldRefill;  // one already runs
            return Result::Applied;
        }

        Result OpCamo(Ctx& c, const char** why) {
            if (!N.camo(c.player, 0, fx::kCamoTicks)) { *why = "camo already active"; return Result::Deferred; }
            return Result::Applied;
        }

        Result OpShieldsDown(Ctx& c, const char**) {
            *reinterpret_cast<float*>(c.u + kObjShield) = 0.0f;
            *reinterpret_cast<int16_t*>(c.u + kObjStun) = static_cast<int16_t>(fx::kShieldsDownStunTicks);
            *reinterpret_cast<uint16_t*>(c.u + kObjFlags) &= static_cast<uint16_t>(~0x1010);  // no recharge, no overshield refill
            if (N.shieldDepleted) N.shieldDepleted(c.unit);  // the engine's own "shield depleted" feedback
            return Result::Applied;
        }

        Result OpDrop(Op op, Ctx& c, const char** why) {
            const fx::Chain chain = fx::ChainFor(op);
            int32_t found[8];
            if (chain.count == 0 || chain.count > 8) { *why = "no weapon chain"; return Result::Failed; }
            FindTags(fx::kClassWeapon, chain.names, chain.count, found);
            const int pick = fx::PickFromChain(chain, [&](size_t i) { return found[i] != -1; });
            if (pick < 0) { *why = "no player weapon in this map"; return Result::Failed; }
            float pos[3] = {}, fwd[3] = {}, up[3] = {};
            N.objPos(c.unit, pos);
            N.objFwd(c.unit, fwd, up);
            float dx = fwd[0], dy = fwd[1];
            const float len = std::sqrt(dx * dx + dy * dy);
            if (len > 1e-4f) { dx /= len; dy /= len; } else { dx = 1.0f; dy = 0.0f; }
            alignas(16) uint8_t buf[0x100];
            std::memset(buf, 0, sizeof(buf));
            N.placeInit(buf, static_cast<uint32_t>(found[pick]), -1);  // owner -1: lies on the ground
            *reinterpret_cast<float*>(buf + 0x1C) = pos[0] + dx * kDropDistance;
            *reinterpret_cast<float*>(buf + 0x20) = pos[1] + dy * kDropDistance;
            *reinterpret_cast<float*>(buf + 0x24) = pos[2] + kDropHeight;
            const uint32_t h = N.placeCreate(buf);
            if (h == 0xFFFFFFFFu) { *why = "the engine refused the spawn"; return Result::Failed; }
            if (!FillWeapon(h)) { *why = "dropped weapon not valid"; return Result::Failed; }
            std::snprintf(g_lastDrop, sizeof(g_lastDrop), "%s", chain.names[pick]);
            printf("[effects] drop: %s -> \"%s\"%s\n", fx::OpLabel(op), chain.names[pick], pick == 0 ? "" : " (fallback: the wanted weapon is not in this map)");
            return Result::Applied;
        }

        Result Execute(Op op, const char** why) {
            if (!NativeOpAvailable(op)) { *why = "native missing in this build"; return Result::Unsupported; }
            Ctx c;
            if (const char* bad = Validate(c, fx::NeedsOnFoot(op))) { *why = bad; return Result::Deferred; }
            switch (op) {
            case Op::SniperDrop: case Op::ShotgunDrop: case Op::MagnumDrop: case Op::RocketDrop: return OpDrop(op, c, why);
            case Op::Overshield: return OpOvershield(c, why);
            case Op::ActiveCamo: return OpCamo(c, why);
            case Op::GrenadePack: return OpGrenadePack(c, why);
            case Op::AmmoRefill: return OpAmmoRefill(c, why);
            case Op::ShieldsDown: return OpShieldsDown(c, why);
            default: *why = "unknown op"; return Result::Failed;
            }
        }

        // The cue after a successful op (once per effect). Its own SEH: a sound problem must not undo an applied effect.
        void PlayCue(Op op) {
            if (!N.sound || !N.tagIter || !N.tagName) return;
            const char* names[1] = { fx::CueFor(op) };
            int32_t id = -1;
            __try {
                FindTags(fx::kClassSound, names, 1, &id);
                if (id != -1) N.sound(id, -1, 1.0f);  // M3: only a tag found by name in class snd!
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                printf("[effects] cue failed (exception)\n");
            }
        }

        void RunOp(Op op, Result* r, const char** why) {
            __try {
                *r = Execute(op, why);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                *r = Result::Failed;
                *why = "exception inside the op";
            }
        }

        // The gateway: replaces the evaluate of `cheat_spawn_warthog`. (void evaluate(int64 unused, u32 thread handle))
        void DetourGateway(void* /*unused*/, uint32_t thread) {
            Op op = Op::None;
            if (g_slot.TryStart(&op)) {
                if (g_threadLogs < 3) { g_threadLogs++; printf("[effects] gateway runs on thread %lu\n", GetCurrentThreadId()); }
                Result r = Result::Failed;
                const char* why = "";
                RunOp(op, &r, &why);
                if (r == Result::Applied) PlayCue(op);
                g_slot.Finish(r, why);
            }
            // QA M2: always pop the HS stack, after the guarded part; the engine loop calls this evaluate again while it is not empty
            if (PopStackFn pop = N.popStack) pop(thread, 0);
        }

    }

    bool NativeOpAvailable(Op op) {
        if (op <= Op::None || op >= Op::Count) return false;
        return (g_available.load(std::memory_order_acquire) >> static_cast<int>(op)) & 1u;
    }

    fx::OpSlot& NativeSlot() { return g_slot; }
    const char* NativeLastDropName() { return g_lastDrop; }

    namespace {
        std::atomic<bool> g_attempted{ false };
    }

    static bool InstallImpl();

    bool InstallNativeEffects() {
        const bool ok = InstallImpl();
        g_attempted.store(true, std::memory_order_release);
        return ok;
    }

    bool NativeInstallAttempted() { return g_attempted.load(std::memory_order_acquire); }

    static bool InstallImpl() {
        if (g_hooked.load()) return true;
        HMODULE h = GetModuleHandleA("halo1.dll");
        if (!h) return false;
        g_available.store(0);
        N = Natives{};
        g_slot.Reset();
        g_lastDrop[0] = 0;

        auto spawn = Find(h, kPatSpawnWarthog, "SpawnWarthog");
        auto pop = Find(h, kPatPopStack, "PopStack");
        auto players = Find(h, kPatPlayerHandle, "PlayerHandle");
        auto datum = Find(h, kPatDatum, "Datum");
        auto object = Find(h, kPatObject, "Object");
        auto cinematic = Find(h, kPatCinematic, "Cinematic");
        if (!spawn || !pop || !players || !datum || !object || !cinematic) {
            printf("[effects-native] core signatures missing: native effect items are off this session\n");
            return false;
        }
        N.popStack = reinterpret_cast<PopStackFn>(const_cast<uint8_t*>(pop));
        N.playerHandle = reinterpret_cast<PlayerHandleFn>(const_cast<uint8_t*>(players));
        N.datum = reinterpret_cast<DatumFn>(const_cast<uint8_t*>(datum));
        N.object = reinterpret_cast<ObjectFn>(const_cast<uint8_t*>(object));
        N.playersVar = ResolveRipLoad(h, players, kPlayersLoadOff);
        N.cinematicVar = reinterpret_cast<uint8_t**>(ResolveRipLoad(h, cinematic, kCinematicLoadOff));
        if (!N.playersVar || !N.cinematicVar) {
            printf("[effects-native] player / cutscene globals not resolved (instruction layout changed): native effect items are off\n");
            N = Natives{};
            return false;
        }
        auto resolve = [&](const char* pat, const char* name) { return const_cast<uint8_t*>(Find(h, pat, name)); };
        N.placeInit = reinterpret_cast<PlaceInitFn>(resolve(kPatPlaceInit, "PlaceInit"));
        N.placeCreate = reinterpret_cast<PlaceCreateFn>(resolve(kPatPlaceCreate, "PlaceCreate"));
        N.objPos = reinterpret_cast<ObjPosFn>(resolve(kPatObjPos, "ObjPos"));
        N.objFwd = reinterpret_cast<ObjFwdFn>(resolve(kPatObjFwd, "ObjFwd"));
        N.tagData = reinterpret_cast<TagDataFn>(resolve(kPatTagData, "TagData"));
        N.tagName = reinterpret_cast<TagNameFn>(resolve(kPatTagName, "TagName"));
        N.tagIter = reinterpret_cast<TagIterFn>(resolve(kPatTagIter, "TagIter"));
        N.overshield = reinterpret_cast<OvershieldFn>(resolve(kPatOvershield, "Overshield"));
        N.shieldDepleted = reinterpret_cast<UnitFn>(resolve(kPatShieldDepleted, "ShieldDepleted"));
        N.camo = reinterpret_cast<CamoFn>(resolve(kPatCamo, "Camo"));
        N.sound = reinterpret_cast<SoundFn>(resolve(kPatSound, "Sound"));
        N.setAmmo = reinterpret_cast<SetAmmoFn>(resolve(kPatSetAmmo, "SetAmmo"));
        N.battery = reinterpret_cast<BatteryFn>(resolve(kPatBattery, "Battery"));
        N.weaponMark = reinterpret_cast<UnitFn>(resolve(kPatWeaponMark, "WeaponMark"));

        // the gateway: from the HS table entry, then verified against the two signatures it must call / jump to
        const uint8_t* ev = ResolveGatewayEvaluate(h);
        if (!ev || !VerifyGatewayBody(ev, spawn, pop)) {
            printf("[effects-native] gateway not found or its body is not the expected one: native effect items are off\n");
            N = Natives{};
            return false;
        }
        if (MH_CreateHook(const_cast<uint8_t*>(ev), reinterpret_cast<void*>(DetourGateway), nullptr) != MH_OK ||
            MH_EnableHook(const_cast<uint8_t*>(ev)) != MH_OK) {
            printf("[effects-native] MinHook failed on the gateway: native effect items are off\n");
            MH_RemoveHook(const_cast<uint8_t*>(ev));
            N = Natives{};
            return false;
        }
        g_gatewayTarget = const_cast<uint8_t*>(ev);
        g_hooked.store(true);

        const bool ammo = N.tagData && N.setAmmo && N.battery && N.weaponMark;
        const bool drop = ammo && N.tagIter && N.tagName && N.placeInit && N.placeCreate && N.objPos && N.objFwd;
        uint32_t mask = 0;
        auto set = [&](Op op, bool ok) { if (ok) mask |= 1u << static_cast<int>(op); };
        set(Op::SniperDrop, drop); set(Op::ShotgunDrop, drop); set(Op::MagnumDrop, drop); set(Op::RocketDrop, drop);
        set(Op::AmmoRefill, ammo);
        set(Op::Overshield, N.overshield != nullptr);
        set(Op::ActiveCamo, N.camo != nullptr);
        set(Op::GrenadePack, true);
        set(Op::ShieldsDown, true);  // shieldDepleted is optional
        g_available.store(mask, std::memory_order_release);
        printf("[effects-native] gateway at %p, op mask %x, sound cue %s\n", ev, mask, (N.sound && N.tagIter && N.tagName) ? "on" : "off");
        for (int i = 1; i < static_cast<int>(Op::Count); i++)
            if (!((mask >> i) & 1u)) printf("[effects-native] op %s disabled (missing native)\n", fx::OpLabel(static_cast<Op>(i)));
        return true;
    }

    void UninstallNativeEffects() {
        g_attempted.store(false, std::memory_order_release);
        g_available.store(0, std::memory_order_release);
        if (g_hooked.exchange(false) && g_gatewayTarget) {
            MH_DisableHook(g_gatewayTarget);
            MH_RemoveHook(g_gatewayTarget);
        }
        g_gatewayTarget = nullptr;
        g_slot.Reset();
    }
}
