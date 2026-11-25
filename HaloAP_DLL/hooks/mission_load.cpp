#include "mission_load.h"
#include "../minhook/MinHook.h"
#include "../pattern_scan.h"
#include "shell_command.h"
#include "skull_hook.h"
#include "effects.h"
#include "shared/common.h"
#include "shared/difficulty.h"
#include <cstdint>
#include <cstdio>
#include <intrin.h>
#include <psapi.h>

#include "../item_handler.h"

#pragma comment(lib, "psapi.lib")

namespace haloap {

    namespace {
        const char* kPattern =
            "40 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 B0 EE FF FF B8 50 12 00 00 E8 ? ? ? ?";

        // Campaign difficulty (PLAN 5.5), read-only. The pattern is the `mov rax,[game globals]` at the start of the
        // difficulty switch in encounter_place (0xbe0d54), followed by `movzx ecx, word [rax+0x158]` (V: unique in halo1.dll).
        const char* kDifficultyPattern =
            "48 8B 05 ? ? ? ? 41 0F B7 F9 0F B7 88 58 01 00 00 66 85 C9 78 ? B8 01 00 00 00 66 3B C8 7E";
        const uint8_t* const* g_globalsVar = nullptr;  // the global that holds the game globals pointer; null = difficulty unavailable

        // Plain C function (no C++ objects) so __try is allowed. -1 = unreadable. Never writes.
        int ReadDifficulty() {
            if (!g_globalsVar) return -1;
            __try {
                return DifficultyFromGlobals(*g_globalsVar);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                return -1;
            }
        }

        void ResolveDifficulty(HMODULE halo1) {
            g_globalsVar = nullptr;
            int hits = 0;
            const uint8_t* p = static_cast<const uint8_t*>(FindUniquePatternInModule(halo1, kDifficultyPattern, &hits));
            if (!p) {
                printf("[hook] (mission_load) difficulty signature %s; difficulty not logged\n", hits ? "ambiguous" : "not found");
                return;
            }
            int32_t disp;
            memcpy(&disp, p + 3, sizeof(disp));
            const uint8_t* var = p + 7 + disp;
            auto base = reinterpret_cast<const uint8_t*>(halo1);
            auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
            if (var < base || var + sizeof(void*) > base + nt->OptionalHeader.SizeOfImage) {
                printf("[hook] (mission_load) difficulty global outside the module image; difficulty not logged\n");
                return;
            }
            g_globalsVar = reinterpret_cast<const uint8_t* const*>(var);
            printf("[hook] (mission_load) difficulty global at %p\n", static_cast<const void*>(var));
        }

        typedef void (*BeginMissionLoadFn)(void* p1, void* p2, const char* path,
            unsigned int p4, char p5);

        BeginMissionLoadFn g_originalBeginMissionLoad = nullptr;
        void* g_hookTarget = nullptr;
        PipeClient* g_pipe = nullptr;
        
        int MissionCodeToIndex(const char* code) {
            struct { const char* code; int index; } mapping[] = {
                { "a10", 0 }, { "a30", 1 }, { "a50", 2 },
                { "b30", 3 }, { "b40", 4 }, { "c10", 5 },
                { "c20", 6 }, { "c40", 7 }, { "d20", 8 },
                { "d40", 9 },
            };
            for (auto& m : mapping) {
                if (strcmp(code, m.code) == 0) return m.index;
            }
            return -1;
        }
 
        // Extract mission code from path like "levels\a10\a10"
        int GetMissionIndexFromPath(const char* path) {
            if (!path) return -1;
            const char* levelsPrefix = "levels\\";
            const char* pos = strstr(path, levelsPrefix);
            if (!pos) return -1;
            pos += strlen(levelsPrefix);
            char code[16] = {};
            for (int i = 0; i < 15 && pos[i] && pos[i] != '\\'; i++) {
                code[i] = pos[i];
            }
            return MissionCodeToIndex(code);
        }

        // Identify the module containing an address. Returns module name and offset.
        // If not found, returns ("?", 0).
        struct ModuleInfo {
            char name[64];
            size_t offset;
        };

        ModuleInfo IdentifyAddress(void* address) {
            ModuleInfo info{};
            info.name[0] = '?';
            info.name[1] = '\0';
            info.offset = (size_t)address;

            HMODULE hMod = nullptr;
            if (GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCSTR)address, &hMod)) {
                char fullPath[MAX_PATH];
                if (GetModuleFileNameA(hMod, fullPath, sizeof(fullPath))) {
                    // Take just the filename, not the full path.
                    const char* base = strrchr(fullPath, '\\');
                    base = base ? base + 1 : fullPath;
                    strncpy_s(info.name, sizeof(info.name), base, _TRUNCATE);
                    info.offset = (size_t)address - (size_t)hMod;
                }
            }
            return info;
        }

        void DetourBeginMissionLoad(void* p1, void* p2, const char* path,
            unsigned int p4, char p5) {
            haloap::SetInMission(true);
            haloap::EffectsSetInMission(true);
            // Walk up the stack. Skip 1 frame (this detour function itself).
            // Capture up to 10 frames after that.
            //constexpr ULONG kFramesToSkip = 1;
            //constexpr ULONG kFramesToCapture = 32;
            //void* stackFrames[kFramesToCapture] = {};
            //USHORT framesCaptured = RtlCaptureStackBackTrace(
            //    kFramesToSkip, kFramesToCapture, stackFrames, nullptr);

            int missionIdx = GetMissionIndexFromPath(path);
            bool isLocked = (missionIdx >= 0 && !GetItemHandler().isMissionAllowed(missionIdx));

            if (g_originalBeginMissionLoad)
            {
                g_originalBeginMissionLoad(p1, p2, path, p4, p5);
            }

            // Log the load itself, after the original so the game globals already hold this load's difficulty
            // (R: confirm in game that a first load after changing difficulty reports the new value).
            // " diff=<n>" is the difficulty, -1 = unreadable (PLAN 5.5).
            char buf[1024];
            snprintf(buf, sizeof(buf),
                "MISSION_LOAD: path='%s' diff=%d", path ? path : "(null)", ReadDifficulty());
            printf("[hook] %s\n", buf);
            if (g_pipe && g_pipe->IsConnected()) {
                g_pipe->SendAsync(buf);
            }

            // ALWAYS call original.
            if (isLocked)
            {
                printf("[hook] Loaded locked mission %d, flagging quit to menu\n", missionIdx);
                g_quitLockedMission.store(true);
            }
            
            
            // Log each captured stack frame.
            //for (USHORT i = 0; i < framesCaptured; i++) {
            //    ModuleInfo info = IdentifyAddress(stackFrames[i]);
            //    char frameBuf[256];
            //    snprintf(frameBuf, sizeof(frameBuf),
            //        "  STACK[%u] %s+0x%zx",
            //        i, info.name, info.offset);
            //    printf("[hook] %s\n", frameBuf);
            //    if (g_pipe && g_pipe->IsConnected()) {
            //        char pipeMsg[300];
            //        snprintf(pipeMsg, sizeof(pipeMsg),
            //            "MISSION_LOAD_FRAME: %u %s+0x%zx",
            //            i, info.name, info.offset);
            //        g_pipe->SendAsync(pipeMsg);
            //    }
            //}
        }
    }

    bool InstallMissionLoadHook(PipeClient* pipe) {
        g_pipe = pipe;

        HMODULE halo1 = GetModuleHandleA("halo1.dll");
        if (!halo1) {
            printf("[hook] (mission_load) halo1.dll not loaded yet\n");
            return false;
        }

        void* target = FindPatternInModule(halo1, kPattern);
        if (!target) {
            printf("[hook] (mission_load) pattern not found\n");
            return false;
        }

        ResolveDifficulty(halo1);  // optional: a miss only means no `diff=` on the message
        g_hookTarget = target;
        printf("[hook] (mission_load) BeginMissionLoad found at %p\n", target);

        MH_STATUS status = MH_CreateHook(target, (void*)DetourBeginMissionLoad,
            (void**)&g_originalBeginMissionLoad);
        if (status != MH_OK) {
            printf("[hook] (mission_load) MH_CreateHook failed: %d\n", status);
            return false;
        }

        status = MH_EnableHook(target);
        if (status != MH_OK) {
            printf("[hook] (mission_load) MH_EnableHook failed: %d\n", status);
            return false;
        }

        printf("[hook] (mission_load) installed.\n");
        return true;
    }

    void UninstallMissionLoadHook() {
        if (g_hookTarget) {
            MH_DisableHook(g_hookTarget);
            MH_RemoveHook(g_hookTarget);
            g_hookTarget = nullptr;
        }
        g_originalBeginMissionLoad = nullptr;
        g_globalsVar = nullptr;
        g_pipe = nullptr;
    }

}  // namespace haloap