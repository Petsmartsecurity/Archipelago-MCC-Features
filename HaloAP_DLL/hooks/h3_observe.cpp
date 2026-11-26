#include "h3_observe.h"
#include "h3_observe_logic.h"
#include "module_observer.h"
#include <intrin.h>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstring>

// Verified against halo3.dll (build in data/build_fingerprint.json) by tools/shared/check_hooks.py (gate: hook-symbols).
// Evidence: docs/research/h3-base-hooks.md, h3-kill-path.md,
// data/halo3/h3_base_hooks.json, docs/stage-reports/phase8-h3-1-base-hooks-qa.md.
// Observe only: every detour reads, formats, queues a message and calls the original exactly once with the arguments it got.
namespace haloap {

    namespace {
        // ---- signatures (function prologues, address operands wildcarded; identical to data/halo3/h3_base_hooks.json) ----
        // main_game_start(game_options*) -> bool: post-hook; the primary load / map-reset event.
        const char* kStartPattern =
            "4C 8B DC 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 80 00 00 00 49 C7 43 88 FE FF FF FF 49 89 5B 18 48 8B F9 "
            "48 8B 05 ? ? ? ? 44 0F B7 40 58";
        // main_game_change_immediate(game_options* or NULL) -> bool: every load / quit-to-menu goes through it.
        const char* kChangePattern =
            "48 8B C4 57 41 54 41 55 41 56 41 57 48 83 EC 60 48 C7 40 A8 FE FF FF FF 48 89 58 08 48 89 68 18 48 89 70 20 "
            "48 8B F9 33 DB E8 ? ? ? ? 89 84";
        // revert_handler(): checkpoint revert request consumer.
        const char* kRevertPattern =
            "48 8B C4 48 89 ? ? 48 89 ? ? 48 89 ? ? 55 41 55 41 56 48 8B EC 48 83 EC 70 0F B6 0D ? ? ? ? 0F 29 ? ? E8 ? ? ? ? "
            "65 48 8B 04 25";
        // restart_handler(): restart-mission request consumer.
        const char* kRestartPattern =
            "40 53 48 83 EC 20 E8 ? ? ? ? 84 C0 74 ? E8 ? ? ? ? 84 C0 75 ? 48 83 3D ? ? ? ? 00 74 ? 38 05 ? ? ? ? 75 ? 38 05 "
            "? ? ? ?";
        // game_won impl(): sets the level-won byte G[0xfb80]; no once-only guard of its own.
        const char* kWonPattern =
            "48 89 5C ? ? 48 89 74 ? ? 57 48 83 EC 20 E8 ? ? ? ? 33 FF 84 C0 0F 84 ? ? ? ? 8B 0D ? ? ? ? 65 48 8B 04 25 58 "
            "00 00 00 BE 48 00";
        // cinematic_set_title impl(ushort index, float duration): chapter title. A RIP-relative load sits inside the 5 patched
        // bytes (QA 8.H3.1 m4); MinHook relocates it.
        const char* kTitlePattern =
            "48 83 EC 28 8B 15 ? ? ? ? 45 33 C0 65 48 8B 04 25 58 00 00 00 41 B9 A8 00 00 00 48 8B 04 D0 4D 8B 0C 01 49 0F "
            "BF C0 48 8D 14 40 66 41 83 7C";
        // engine slot 3 (engine, int msg, void* ctx): host shell messages. Runs on an MCC thread: no game TLS, ctx never read.
        const char* kShellPattern =
            "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B F1 49 8B F8 48 81 C1 40 04 00 00 8B DA FF 15 ? ? ? ? 48 8B "
            "D0 48 85 C0 74 ? 48 83 60 10";
        // actor_died(actor handle): real deaths (from unit_die) and ai_kill actor kills.
        const char* kDiedPattern =
            "89 4C ? ? 53 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 38 8B 15 ? ? ? ? 65 48 8B 04 25 58 00 00 00 41 B8 08 00 "
            "00 00 0F 29 7C ? ? 44 8B";
        // actor_delete(actor handle, died): died == 0 is an erase / despawn; died == 1 comes from actor_died only.
        const char* kDeletePattern =
            "88 54 24 10 55 53 56 57 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 48 44 8B 05 ? ? ? ? 41 83 C9 FF";
        // ---- not hooked: anchors only ----
        // main_game_reset_map(char): its call to main_game_start is the map-reset return address.
        const char* kResetMapPattern =
            "40 55 57 41 54 41 56 41 57 48 8D AC 24 80 04 FF FF B8 80 FC 00 00 E8 ? ? ? ? 48 2B E0 48 C7 44 24 58 FE FF FF "
            "FF 48 89 9C 24 C0 FC 00 00 48";
        // main_game_change_update(): per-frame queue consumer (queued loads, the next mission after a win).
        const char* kUpdatePattern =
            "40 56 48 83 EC 20 80 3D ? ? ? ? 00 0F 84 ? ? ? ? E8 ? ? ? ? 84 C0 0F 85 ? ? ? ? E8 ? ? ? ? 84 C0 74 ? BA 04 00 "
            "00 00 48 8D";
        // checkpoint_options_apply(): a checkpoint "continue" into other options calls change_immediate from here.
        const char* kCkptPattern =
            "48 8B C4 48 89 ? ? 48 89 ? ? 48 89 ? ? 48 89 ? ? 41 56 48 83 EC 20 BF 01 00 00 00 48 8D 99 38 01 00 00 40 88 B9 "
            "39 01 00 00 4D 8B F1 C6";
        // main_game_return_to_menu(): builds a mode-3 options block and calls change_immediate.
        const char* kMenuPattern =
            "40 53 B8 90 FB 00 00 E8 ? ? ? ? 48 2B E0 48 C7 44 24 20 FE FF FF FF E8 ? ? ? ? 8B D8 89 84 24 A0 FB 00 00 E8 ? "
            "? ? ? 33 C9 E8 ? ?";
        // game_difficulty_get_real_impl(): its first instruction loads the DLL's own _tls_index; never hooked.
        const char* kDiffPattern =
            "8B 0D ? ? ? ? 65 48 8B 04 25 58 00 00 00 BA 48 00 00 00 48 8B 04 C8 48 8B 0C 02 B8 01 00 00 00 38 41 10 75 ? 0F "
            "B7 81 6C 01 00 00 C3 CC CC";
        // unit_die(): only its call to actor_died is used (kill discriminator); not hooked.
        const char* kUnitDiePattern =
            "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 41 54 41 55 41 56 41 57 48 81 EC C0 01 00 00 44 8B 15 ? ? ? ?";
        // ai_kill's per-actor worker: only its call to actor_died is used (script-kill discriminator); not hooked.
        const char* kAiKillPattern =
            "48 89 5C ? ? 48 89 6C ? ? 48 89 74 ? ? 57 48 83 EC 20 44 8B 0D ? ? ? ? 40 8A FA 65 48 8B 04 25 58 00 00 00 41 "
            "8A E8";

        // RIP-relative operand inside a matched function: offset of the instruction, opcode bytes (opLen of them), disp32 follows
        // the opcode, instruction length = opLen + 4 (+ 0 for these loads / stores).
        struct RipOp { size_t offset; uint8_t op[3]; size_t opLen; };
        constexpr RipOp kTlsIndexOp = { 0x0, { 0x8B, 0x0D, 0x00 }, 2 };   // difficulty impl: mov ecx,[_tls_index]
        constexpr RipOp kScnPtrOp   = { 0x24, { 0x48, 0x8B, 0x05 }, 3 };  // main_game_start: mov rax,[scenario tag pointer]

        // Call-site facts: (function start + offset) = E8 rel32 to the stated callee. Checked at install, fail closed; re-verified
        // offline by tests/test_h3_observe.py.
        constexpr size_t kStartFromChangeCall = 0x386;  // main_game_change_immediate -> main_game_start   (fresh load)
        constexpr size_t kStartFromResetCall = 0x2A6;   // main_game_reset_map -> main_game_start          (map reset)
        constexpr size_t kChangeFromUpdateCall = 0x13A; // main_game_change_update -> change_immediate     (queued change)
        constexpr size_t kChangeFromCkptCall = 0x8F;    // checkpoint_options_apply -> change_immediate
        constexpr size_t kChangeFromMenuCall = 0x5C;    // main_game_return_to_menu -> change_immediate
        constexpr size_t kDiedFromUnitDieCall = 0x358;  // unit_die -> actor_died          (real death)
        constexpr size_t kDiedFromAiKillCall = 0xA8;    // ai_kill actor worker -> actor_died (script kill)
        constexpr size_t kCallLen = 5;
        constexpr size_t kStartCallSites = 2;           // main_game_start has exactly two direct callers

        // Struct fields read (V in the research docs; pinned by tests/test_h3_observe.py where the bytes allow).
        constexpr int kOptsMode = 0x00, kOptsCampaign = 0x3C, kOptsMapId = 0x40, kOptsPath = 0x44, kOptsPathChars = 0x104,
                      kOptsZoneSet = 0x148, kOptsDifficulty = 0x15C, kOptsInsertion = 0x15E;
        constexpr int kTlsGlobals = 0x48, kTlsActors = 0x08, kDataArrayElems = 0x48;
        constexpr int kGMode = 0x10, kGDifficulty = 0x16C, kGActive = 0xFB70, kGWon = 0xFB80;
        constexpr int kScnCampaign = 0x08, kScnMapId = 0x0C;
        constexpr int kActorUnit = 0x14, kActorSquad = 0x30, kActorChar = 0x5C, kActorSubA = 0x6C, kActorState = 0x300, kActorSubState = 0x302;
        constexpr uint32_t kActorSize = 0xAA0;
        constexpr uint32_t kWonRepeatLogCap = 4;

        // ---- hook table (order of enum, kPatName, kPatText and kDetours must agree: tests/test_h3_observe.py checks the source) ----
        enum Pat { P_START, P_CHANGE, P_REVERT, P_RESTART, P_WON, P_TITLE, P_SHELL, P_DIED, P_DELETE,
                   P_RESET_MAP, P_UPDATE, P_CKPT, P_MENU, P_DIFF, P_UNIT_DIE, P_AI_KILL, P_COUNT };
        constexpr int kHookCount = P_RESET_MAP;  // the first nine patterns are hook targets
        const char* const kPatName[P_COUNT] = { "game_start", "change_immediate", "revert", "restart", "game_won", "title",
                                                "shell_msg", "actor_died", "actor_delete", "reset_map", "change_update",
                                                "checkpoint_apply", "return_to_menu", "difficulty", "unit_die", "ai_kill" };
        const char** const kPatText[P_COUNT] = { &kStartPattern, &kChangePattern, &kRevertPattern, &kRestartPattern, &kWonPattern,
                                                 &kTitlePattern, &kShellPattern, &kDiedPattern, &kDeletePattern, &kResetMapPattern,
                                                 &kUpdatePattern, &kCkptPattern, &kMenuPattern, &kDiffPattern, &kUnitDiePattern,
                                                 &kAiKillPattern };

        // ---- state ---------------------------------------------------------------------------------------------
        // Anchors written by the worker thread (obs::ModuleObserver commits them before a hook is enabled), read by detours.
        std::atomic<uintptr_t> g_tlsVar{ 0 }, g_scnVar{ 0 };  // addresses of the game's _tls_index and scenario-tag pointer variables
        // absolute return addresses (0 = unknown)
        std::atomic<uintptr_t> g_loadRet{ 0 }, g_resetRet{ 0 }, g_queueRet{ 0 }, g_ckptRet{ 0 }, g_menuRet{ 0 },
                               g_killRet{ 0 }, g_aiKillRet{ 0 };
        std::atomic<int32_t> g_lastShell{ INT32_MIN };
        std::atomic<uint32_t> g_shellRep{ 0 };
        std::atomic<uint32_t> g_wonRep{ 0 };
        std::atomic<uint32_t> g_eraseCount{ 0 };

        // The shared lifecycle (module watch, scan, install, retries, integrity, reattach, uninstall, line numbering, send).
        extern obs::ModuleObserver g_obs;

        // ---- SEH helpers: plain C functions, no objects with destructors ----------------------------------------
        // The RIP-relative operand at fn+offset must carry the expected opcode; returns the address of the variable it refers to,
        // or 0 (fail closed) when the bytes differ or the target is outside the module image.
        uintptr_t ResolveRip(const uint8_t* fn, const RipOp& r, uintptr_t base, uintptr_t size) {
            if (!fn) return 0;
            __try {
                const uint8_t* ins = fn + r.offset;
                if (memcmp(ins, r.op, r.opLen) != 0) return 0;
                int32_t disp;
                memcpy(&disp, ins + r.opLen, sizeof(disp));
                auto target = reinterpret_cast<uintptr_t>(ins + r.opLen + 4 + disp);
                return (target >= base && target < base + size) ? target : 0;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                return 0;
            }
        }

        // fn+off is `E8 rel32` to `callee`: returns the return address (fn+off+5), else 0.
        uintptr_t VerifiedRet(const uint8_t* fn, size_t off, const uint8_t* callee) {
            if (!fn || !callee || !obs::CallAtTargets(fn + off, callee)) return 0;
            return reinterpret_cast<uintptr_t>(fn + off + kCallLen);
        }

        // ---- reads done inside the detours (all under SEH; never writes) --------------------------------------------
        // This thread's TLS block of halo3.dll: tls[_tls_index]. Only valid on a game thread (the MCC-owned shell thread has none).
        bool TlsBlock(const uint8_t** block) {
            uintptr_t var = g_tlsVar.load(std::memory_order_relaxed);
            if (!var) return false;
            __try {
                uint32_t idx = *reinterpret_cast<const uint32_t*>(var);
                auto tls = reinterpret_cast<void* const*>(__readgsqword(0x58));
                if (!tls) return false;
                *block = static_cast<const uint8_t*>(tls[idx]);
                return *block != nullptr;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        struct StartInfo {
            bool ok = false;
            int mode = -1, camp = -1, map = -1, zs = -1, diff = -1, ins = -1;
            char path[40] = {};
        };
        void ReadStart(const uint8_t* opts, StartInfo* out) {
            if (!opts) return;
            __try {
                out->mode = *(opts + kOptsMode);
                out->camp = *reinterpret_cast<const int32_t*>(opts + kOptsCampaign);
                out->map = *reinterpret_cast<const int32_t*>(opts + kOptsMapId);
                out->zs = *reinterpret_cast<const int16_t*>(opts + kOptsZoneSet);
                out->diff = *reinterpret_cast<const int16_t*>(opts + kOptsDifficulty);
                out->ins = *reinterpret_cast<const int16_t*>(opts + kOptsInsertion);
                char p[kOptsPathChars + 1];
                int n = 0;
                for (; n < kOptsPathChars; n++) {
                    p[n] = *reinterpret_cast<const char*>(opts + kOptsPath + n);
                    if (!p[n]) break;
                }
                p[n] = 0;
                h3::SanitizePathTail(p, static_cast<size_t>(n), out->path, sizeof(out->path));
                out->ok = true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }

        struct GInfo {
            bool ok = false;
            int mode = -1, diff = -1, active = -1, won = -1;
        };
        void ReadGlobals(GInfo* out) {
            const uint8_t* block;
            if (!TlsBlock(&block)) return;
            __try {
                const uint8_t* g = *reinterpret_cast<const uint8_t* const*>(block + kTlsGlobals);
                if (!g) return;
                out->mode = *(g + kGMode);
                out->diff = *reinterpret_cast<const int16_t*>(g + kGDifficulty);
                out->active = *(g + kGActive);
                out->won = *(g + kGWon);
                out->ok = true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }

        // The loaded scenario tag's campaign / map id (the second, independent identity source).
        bool ReadScenario(int* camp, int* map) {
            uintptr_t var = g_scnVar.load(std::memory_order_relaxed);
            if (!var) return false;
            __try {
                const uint8_t* s = *reinterpret_cast<const uint8_t* const*>(var);
                if (!s) return false;
                *camp = *reinterpret_cast<const int32_t*>(s + kScnCampaign);
                *map = *reinterpret_cast<const int32_t*>(s + kScnMapId);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        struct ActorInfo {
            bool ok = false, saltOk = false;
            uint32_t unit = 0xFFFFFFFF, squad = 0xFFFFFFFF, ch = 0, suba = 0;
            int state = -1, sub = -1;
        };
        void ReadActor(uint32_t handle, ActorInfo* out) {
            if (handle == 0xFFFFFFFFu) return;
            const uint8_t* block;
            if (!TlsBlock(&block)) return;
            __try {
                const uint8_t* hdr = *reinterpret_cast<const uint8_t* const*>(block + kTlsActors);
                if (!hdr) return;
                const uint8_t* data = *reinterpret_cast<const uint8_t* const*>(hdr + kDataArrayElems);
                const uint8_t* a = h3::DataElement(data, handle, kActorSize, &out->saltOk);
                if (!a) return;
                out->unit = *reinterpret_cast<const uint32_t*>(a + kActorUnit);
                out->squad = *reinterpret_cast<const uint32_t*>(a + kActorSquad);
                out->ch = *reinterpret_cast<const uint32_t*>(a + kActorChar);
                out->suba = *reinterpret_cast<const uint32_t*>(a + kActorSubA);
                out->state = *reinterpret_cast<const int16_t*>(a + kActorState);
                out->sub = *reinterpret_cast<const int16_t*>(a + kActorSubState);
                out->ok = true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                out->ok = false;
            }
        }

        // ---- message plumbing (a bounded snprintf and SendAsync, nothing else) -------------------------------------
        obs::Msg NewMsg(const char* ev) { return g_obs.NewMsg(ev); }
        void Send(obs::Msg& m) { g_obs.Send(m); }
        void AddRet(obs::Msg& m, const char* key, uintptr_t addr) { g_obs.AddRet(m, key, addr); }

        void AddScenario(obs::Msg& m) {
            int c, id;
            if (ReadScenario(&c, &id)) m.Add("scn=%d/%d", c, id);
            else m.Add("scn=-");
        }

        template <class Fn>
        Fn Orig(int idx) { return reinterpret_cast<Fn>(g_obs.Orig(idx)); }

        // ---- detours -------------------------------------------------------------------------------------------
        typedef bool (*StartFn)(void*);
        typedef bool (*ChangeFn)(void*);
        typedef void (*VoidFn)();
        typedef void (*TitleFn)(uint16_t, float);
        typedef void (*ShellFn)(void*, int, void*);
        typedef void (*DiedFn)(uint32_t);
        typedef uint64_t (*DeleteFn)(uint64_t, uint64_t, uint64_t, uint64_t);

        // Post-hook: the options pointer is the load being started (its difficulty is read from the argument, never from the
        // game globals that still hold the previous load's value until the original has run: QA phase5.5 M1).
        bool DetourStart(void* opts) {
            const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
            StartInfo s;
            ReadStart(static_cast<const uint8_t*>(opts), &s);  // before: the original must not be able to change what we report
            const bool r = Orig<StartFn>(P_START)(opts);
            h3::StartKind kind = h3::ClassifyStart(ret, g_loadRet.load(std::memory_order_relaxed), g_resetRet.load(std::memory_order_relaxed));
            if (kind != h3::StartKind::Other) g_eraseCount.store(0, std::memory_order_relaxed);
            GInfo g;
            ReadGlobals(&g);
            obs::Msg m = NewMsg("start");
            m.Add("kind=%s ok=%d", h3::StartKindName(kind), r ? 1 : 0);
            AddRet(m, "ret", ret);
            if (s.ok) m.Add("mode=%d camp=%d map=%d diff=%d zs=%d ins=%d", s.mode, s.camp, s.map, s.diff, s.zs, s.ins);
            else m.Add("mode=- camp=- map=- diff=- zs=- ins=-");
            m.Add("gdiff=%d gmode=%d", g.ok ? g.diff : -1, g.ok ? g.mode : -1);
            AddScenario(m);
            m.Add("path=%s", s.path[0] ? s.path : "-");
            Send(m);
            return r;
        }

        // Pre-hook of every load / quit-to-menu: logs the request (and its caller) before the engine acts on it.
        bool DetourChange(void* opts) {
            const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
            StartInfo s;
            ReadStart(static_cast<const uint8_t*>(opts), &s);
            h3::ChangeSrc src = h3::ClassifyChangeSrc(ret, g_queueRet.load(std::memory_order_relaxed),
                                                      g_ckptRet.load(std::memory_order_relaxed), g_menuRet.load(std::memory_order_relaxed));
            obs::Msg m = NewMsg("change");
            m.Add("src=%s", h3::ChangeSrcName(src));
            AddRet(m, "ret", ret);
            if (!opts) m.Add("null=1 quit=1");
            else if (s.ok) {
                m.Add("mode=%d camp=%d map=%d diff=%d", s.mode, s.camp, s.map, s.diff);
                if (h3::IsQuit(false, s.mode)) m.Add("quit=1");
                m.Add("path=%s", s.path[0] ? s.path : "-");
            } else m.Add("mode=- bad=1");
            Send(m);
            return Orig<ChangeFn>(P_CHANGE)(opts);
        }

        void SendSignal(const char* ev, uintptr_t ret) {
            GInfo g;
            ReadGlobals(&g);
            obs::Msg m = NewMsg(ev);
            AddRet(m, "ret", ret);
            m.Add("gdiff=%d gmode=%d", g.ok ? g.diff : -1, g.ok ? g.mode : -1);
            AddScenario(m);
            Send(m);
        }

        // Revert also runs for the game_revert of a skipped cutscene: log only.
        void DetourRevert() {
            SendSignal("revert", reinterpret_cast<uintptr_t>(_ReturnAddress()));
            Orig<VoidFn>(P_REVERT)();
        }

        void DetourRestart() {
            SendSignal("restart", reinterpret_cast<uintptr_t>(_ReturnAddress()));
            Orig<VoidFn>(P_RESTART)();
        }

        void DetourWon() {
            const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
            GInfo g;
            ReadGlobals(&g);  // before: the original sets G[0xfb80]
            Orig<VoidFn>(P_WON)();
            const int was = g.ok ? g.won : -1;
            const uint32_t rep = was == 0 ? 0 : g_wonRep.load(std::memory_order_relaxed);
            const h3::WonDecision d = h3::DecideWon(was, rep, kWonRepeatLogCap);
            if (d.first) g_wonRep.store(0, std::memory_order_relaxed);
            else g_wonRep.fetch_add(1, std::memory_order_relaxed);
            if (!d.log) return;
            obs::Msg m = NewMsg("won");
            m.Add("first=%d was=%d", d.first ? 1 : 0, was);
            if (!d.first) m.Add("rep=%u", rep + 1);
            m.Add("mode=%d gdiff=%d", g.ok ? g.mode : -1, g.ok ? g.diff : -1);
            AddRet(m, "ret", ret);
            AddScenario(m);
            Send(m);
        }

        void DetourTitle(uint16_t idx, float dur) {
            obs::Msg m = NewMsg("title");
            m.Add("idx=%u dur=%.2f", static_cast<unsigned>(idx), static_cast<double>(dur));
            AddScenario(m);
            Send(m);
            Orig<TitleFn>(P_TITLE)(idx, dur);
        }

        void SendShell(int msg, uintptr_t ret, uint32_t rep, bool flush) {
            obs::Msg m = NewMsg("shell");
            m.Add("msg=%d", msg);
            if (ret) AddRet(m, "ret", ret);
            if (rep) m.Add("rep=%u", rep);
            if (flush) m.Add("flush=1");
            Send(m);
        }

        // Engine slot 3 runs on an MCC thread: no game TLS here, and ctx is never touched (it is an MCC-owned pointer for
        // ids 0x10-0x13 / 0x18; QA 8.H3.1 m5). Coalescing as in the Halo 2 set; pause (0/1), UI (2-4), quit (0xD) are never coalesced.
        void DetourShell(void* engine, int msg, void* ctx) {
            const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
            const bool always = msg == 0 || msg == 1 || msg == 2 || msg == 3 || msg == 4 || msg == 0xD;
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
                if (h3::ShellRepeatDue(c)) SendShell(msg, ret, c, false);
            }
            Orig<ShellFn>(P_SHELL)(engine, msg, ctx);
        }

        void AddActor(obs::Msg& m, uint32_t handle, const ActorInfo& a) {
            m.Add("ah=%x", handle);
            if (a.ok) {
                m.Add("unit=%x sq=%x ch=%x sub=%x st=%d/%d%s", a.unit, a.squad, a.ch, a.suba, a.state, a.sub, a.saltOk ? "" : " salt=bad");
            } else {
                m.Add("unit=- bad=1");
            }
        }

        void DetourDied(uint32_t handle) {
            const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
            ActorInfo a;
            ReadActor(handle, &a);  // before: the original frees the actor
            GInfo g;
            ReadGlobals(&g);
            Orig<DiedFn>(P_DIED)(handle);
            h3::DeathKind kind = h3::ClassifyDeath(ret, g_killRet.load(std::memory_order_relaxed), g_aiKillRet.load(std::memory_order_relaxed));
            obs::Msg m = NewMsg("died");
            m.Add("kind=%s", h3::DeathKindName(kind));
            AddRet(m, "src", ret);
            AddActor(m, handle, a);
            m.Add("gdiff=%d", g.ok ? g.diff : -1);
            AddScenario(m);
            Send(m);
        }

        // All four argument registers are passed through untouched; only died == 0 (an erase / despawn) is reported.
        uint64_t DetourDelete(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
            const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
            if (static_cast<uint8_t>(a2) != 0) return Orig<DeleteFn>(P_DELETE)(a1, a2, a3, a4);
            const uint32_t handle = static_cast<uint32_t>(a1);
            const uint32_t count = g_eraseCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (!h3::EraseLogDue(count)) return Orig<DeleteFn>(P_DELETE)(a1, a2, a3, a4);
            ActorInfo a;
            ReadActor(handle, &a);  // before: the original frees the actor
            const uint64_t r = Orig<DeleteFn>(P_DELETE)(a1, a2, a3, a4);
            obs::Msg m = NewMsg("erase");
            AddRet(m, "src", ret);
            AddActor(m, handle, a);
            m.Add("cnt=%u", count);
            Send(m);
            return r;
        }

        void* const kDetours[kHookCount] = { reinterpret_cast<void*>(DetourStart), reinterpret_cast<void*>(DetourChange),
                                             reinterpret_cast<void*>(DetourRevert), reinterpret_cast<void*>(DetourRestart),
                                             reinterpret_cast<void*>(DetourWon), reinterpret_cast<void*>(DetourTitle),
                                             reinterpret_cast<void*>(DetourShell), reinterpret_cast<void*>(DetourDied),
                                             reinterpret_cast<void*>(DetourDelete) };

        // ---- anchors and the observer description (worker thread only) -------------------------------------------
        // Derived anchors are computed into a pending copy (seeded from the published ones) and published by Commit,
        // pinned and before any jump exists.
        struct Derived {
            uintptr_t tlsVar = 0, scnVar = 0, loadRet = 0, resetRet = 0, queueRet = 0, ckptRet = 0, menuRet = 0, killRet = 0, aiKillRet = 0;
        };

        // Fills only the fields that are still 0, from the resolved signature addresses (installed targets included: every
        // anchor sits beyond the 5 patched bytes, or in an unhooked function). Everything under SEH; nothing is committed here.
        void ComputeDerived(HMODULE h, const obs::ModId& id, const uint8_t* const* a, Derived* d, char* notes, size_t noteCap) {
            if (!d->tlsVar) d->tlsVar = ResolveRip(a[P_DIFF], kTlsIndexOp, id.base, id.size);
            if (!d->scnVar) d->scnVar = ResolveRip(a[P_START], kScnPtrOp, id.base, id.size);
            if (!d->killRet) d->killRet = VerifiedRet(a[P_UNIT_DIE], kDiedFromUnitDieCall, a[P_DIED]);
            if (!d->aiKillRet) d->aiKillRet = VerifiedRet(a[P_AI_KILL], kDiedFromAiKillCall, a[P_DIED]);
            if (!d->queueRet) d->queueRet = VerifiedRet(a[P_UPDATE], kChangeFromUpdateCall, a[P_CHANGE]);
            if (!d->ckptRet) d->ckptRet = VerifiedRet(a[P_CKPT], kChangeFromCkptCall, a[P_CHANGE]);
            if (!d->menuRet) d->menuRet = VerifiedRet(a[P_MENU], kChangeFromMenuCall, a[P_CHANGE]);
            // main_game_start has exactly two direct callers: change_immediate (load) and reset_map (reset), both at the stated offsets.
            if (!d->loadRet && a[P_CHANGE] && a[P_RESET_MAP] && a[P_START]) {
                const uint8_t* sites[4] = {};
                size_t n = obs::CallSitesInModule(h, a[P_START], sites, 4);
                uintptr_t l = VerifiedRet(a[P_CHANGE], kStartFromChangeCall, a[P_START]);
                uintptr_t r = VerifiedRet(a[P_RESET_MAP], kStartFromResetCall, a[P_START]);
                if (n == kStartCallSites && l && r) { d->loadRet = l; d->resetRet = r; }
                else snprintf(notes + strlen(notes), noteCap - strlen(notes), "main_game_start has %zu direct callers (expected %zu) or the call offsets differ: starts not classified. ", n, kStartCallSites);
            }
        }

        Derived g_pending;

        void Derive(const obs::DeriveCtx& c) {
            Derived& d = g_pending;
            d.tlsVar = g_tlsVar; d.scnVar = g_scnVar; d.loadRet = g_loadRet; d.resetRet = g_resetRet; d.queueRet = g_queueRet;
            d.ckptRet = g_ckptRet; d.menuRet = g_menuRet; d.killRet = g_killRet; d.aiKillRet = g_aiKillRet;
            ComputeDerived(c.h, c.id, c.addr, &d, c.notes, c.noteCap);
        }

        void Commit() {
            const Derived& d = g_pending;
            g_tlsVar = d.tlsVar; g_scnVar = d.scnVar; g_loadRet = d.loadRet; g_resetRet = d.resetRet; g_queueRet = d.queueRet;
            g_ckptRet = d.ckptRet; g_menuRet = d.menuRet; g_killRet = d.killRet; g_aiKillRet = d.aiKillRet;
        }

        void ClearAnchors() {
            g_tlsVar = 0; g_scnVar = 0;
            g_loadRet = 0; g_resetRet = 0; g_queueRet = 0; g_ckptRet = 0; g_menuRet = 0; g_killRet = 0; g_aiKillRet = 0;
        }

        bool AnchorsMissing() {
            return !g_tlsVar.load() || !g_scnVar.load() || !g_loadRet.load() || !g_resetRet.load() || !g_queueRet.load() ||
                   !g_ckptRet.load() || !g_menuRet.load() || !g_killRet.load() || !g_aiKillRet.load();
        }

        void Report() {
            if (!g_tlsVar.load()) printf("[h3] _tls_index not resolved: no game-globals / actor reads (gdiff=-, unit=-)\n");
            if (!g_loadRet.load()) printf("[h3] main_game_start callers not verified: starts are reported as kind=other\n");
            if (!g_killRet.load()) printf("[h3] unit_die -> actor_died link not verified: deaths are reported as kind=unknown\n");
        }

        void InstallFields(obs::Msg& ins) {
            ins.Add("tls=%d scn=%d load=%d reset=%d queue=%d ckpt=%d menu=%d kill=%d aikill=%d", g_tlsVar.load() ? 1 : 0,
                    g_scnVar.load() ? 1 : 0, g_loadRet.load() ? 1 : 0, g_resetRet.load() ? 1 : 0, g_queueRet.load() ? 1 : 0,
                    g_ckptRet.load() ? 1 : 0, g_menuRet.load() ? 1 : 0, g_killRet.load() ? 1 : 0, g_aiKillRet.load() ? 1 : 0);
        }

        const obs::ObserverSpec kSpec = { h3::kTag, "h3", "halo3.dll", P_COUNT, kHookCount, kPatName, kPatText, kDetours,
                                          nullptr, Derive, Commit, ClearAnchors, AnchorsMissing, Report, InstallFields };
        obs::ModuleObserver g_obs(kSpec);
    }  // namespace

    bool H3Loaded() { return GetModuleHandleA("halo3.dll") != nullptr; }

    void H3Tick(PipeClient* pipe) {
        g_obs.Tick(pipe, GetModuleHandleA("halo3.dll"));  // no refcount: a compare only
    }

    void UninstallH3Observe() {
        g_obs.Uninstall();
    }

}  // namespace haloap
