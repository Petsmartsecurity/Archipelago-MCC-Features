#include "effects.h"
#include "effect_queue.h"
#include "effect_native.h"
#include "shell_level_load.h"
#include <windows.h>
#include <cstdio>
#include <map>
#include <mutex>

namespace haloap {
    namespace {
        // One lock covers the queue, the in-mission flag AND the post itself: the 0xD teardown detour calls
        // EffectsSetInMission(false) before the original, so a teardown waits for a post in flight and no post starts after it
        // (QA M3). Only SendAsync (a queue push) runs under it.
        std::mutex g_mutex;
        fx::EffectQueue g_queue;
        bool g_inMission = false;
        bool g_loadPending = false;  // a mission load happened since the last tick: restart the queue's settle clock

        // Native items (phase B): at most one op in flight (request slot -> gateway detour -> result), polled every tick.
        struct Inflight { bool active = false; fx::Action action; uint64_t startMs = 0; fx::Op op = fx::Op::None; };
        Inflight g_inflight;
        std::map<long, int> g_failCount;        // per AP item index: hard failures (not deferrals) so far
        uint64_t g_lastDeferLogMs = 0;
        bool g_workerThreadLogged = false;
        constexpr int kMaxHardFailures = 5;     // a native op that keeps failing is given up (EFFECT_SKIPPED) instead of retried forever
        constexpr uint64_t kDeferLogMs = 15000; // the queue retries a deferred op every 2 s: log it rarely

        void Skipped(PipeClient* pipe, int id, long index, const char* why) {
            printf("[effects] skipped %d (index %ld): %s\n", id, index, why);
            if (pipe && pipe->IsConnected()) pipe->SendAsync("EFFECT_SKIPPED: " + std::to_string(id) + " " + std::to_string(index));
        }

        void Ack(PipeClient* pipe, int id, long index) {
            if (pipe && pipe->IsConnected()) pipe->SendAsync("EFFECT_APPLIED: " + std::to_string(id) + " " + std::to_string(index));
        }
    }

    void EffectsSetInMission(bool inMission) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (inMission) g_loadPending = true;
        g_inMission = inMission;
    }

    void EffectsOnMessage(PipeClient* pipe, const std::string& message) {
        auto parsed = fx::ParseEffectMessage(message);
        if (!parsed) {
            printf("[effects] malformed message ignored: %.80s\n", message.c_str());
            return;
        }
        auto [id, index] = *parsed;
        std::lock_guard<std::mutex> lock(g_mutex);
        switch (g_queue.Add(id, index)) {
        case fx::Enqueue::Queued: printf("[effects] queued %d (index %ld)\n", id, index); break;
        case fx::Enqueue::Duplicate: break;  // still queued: the launcher re-sends unconfirmed items
        case fx::Enqueue::AlreadyApplied: Ack(pipe, id, index); break;  // its ack may have been lost (QA M4)
        case fx::Enqueue::UnknownId: Skipped(pipe, id, index, "unknown effect id (DLL older than the seed?)"); break;
        case fx::Enqueue::Full:  // not an error and not a skip: the launcher keeps it in flight and offers it again later (QA m3)
            printf("[effects] queue full, %d (index %ld) stays with the launcher\n", id, index);
            break;
        }
    }

    namespace {
        // g_mutex held. Result of the op in flight, or its timeout.
        void PollNative(PipeClient* pipe, uint64_t now) {
            if (!g_inflight.active) return;
            const fx::Action a = g_inflight.action;
            fx::Result r;
            const char* why = "";
            if (NativeSlot().Take(&r, &why)) {
                g_inflight.active = false;
                switch (r) {
                case fx::Result::Applied:
                    printf("[effects] applied %d (native %s)\n", a.id, fx::OpLabel(g_inflight.op));
                    g_failCount.erase(a.index);
                    Ack(pipe, a.id, a.index);
                    break;
                case fx::Result::Deferred:  // the world is not ready (no unit, dead, cutscene, vehicle ...): held, never dropped
                    if (now - g_lastDeferLogMs >= kDeferLogMs || g_lastDeferLogMs == 0) {
                        printf("[effects] %d (%s) waits: %s\n", a.id, fx::OpLabel(g_inflight.op), why);
                        g_lastDeferLogMs = now;
                    }
                    g_queue.Failed(a, now);
                    break;
                case fx::Result::Unsupported:
                    Skipped(pipe, a.id, a.index, why);
                    break;
                case fx::Result::Failed:
                    printf("[effects] %d (%s) FAILED: %s\n", a.id, fx::OpLabel(g_inflight.op), why);
                    if (++g_failCount[a.index] >= kMaxHardFailures) Skipped(pipe, a.id, a.index, "native op keeps failing");
                    else g_queue.Failed(a, now);
                    break;
                }
            } else if (fx::OpTimedOut(now, g_inflight.startMs) && NativeSlot().Cancel()) {
                // never started: the post was lost (hs runtime down, compile error, teardown). Held and retried.
                g_inflight.active = false;
                printf("[effects] %d (%s): the gateway did not run within %d ms, retrying\n", a.id, fx::OpLabel(g_inflight.op),
                       static_cast<int>(fx::kOpTimeoutMs));
                g_queue.Failed(a, now);
            }
        }

        // g_mutex held. Starts a native apply: request slot + the one HS line that makes the engine call our gateway.
        void StartNative(PipeClient* pipe, const fx::Action& a, uint64_t now) {
            const fx::Op op = fx::OpFromCommand(a.command);
            if (op == fx::Op::None) { Skipped(pipe, a.id, a.index, "unknown native op (DLL older than the seed?)"); return; }
            if (!NativeInstallAttempted()) { g_queue.Failed(a, now); return; }  // halo1 hooks are being (re)installed: try again
            if (!NativeOpAvailable(op)) { Skipped(pipe, a.id, a.index, "this MCC build lacks a native this item needs"); return; }
            if (g_inflight.active || !NativeSlot().Request(op)) { g_queue.Failed(a, now); return; }
            if (!g_workerThreadLogged) { g_workerThreadLogged = true; printf("[effects] worker thread %lu posts the gateway\n", GetCurrentThreadId()); }
            if (!PostHaloScript(kGatewayCommand)) {
                NativeSlot().Cancel();
                printf("[effects] POST FAILED %d (retry in %d ms): HS: %s\n", a.id, static_cast<int>(fx::kRetryMs), kGatewayCommand);
                g_queue.Failed(a, now);
                return;
            }
            g_inflight = { true, a, now, op };
        }
    }

    void EffectsTick(PipeClient* pipe) {
        const uint64_t now = GetTickCount64();
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_loadPending) { g_queue.OnMissionLoad(now); g_loadPending = false; }
        PollNative(pipe, now);
        fx::Gate gate;
        gate.inMission = g_inMission;
        gate.engineReady = CanPostHaloScript();
        for (const auto& a : g_queue.Tick(now, gate)) {
            if (a.kind == fx::Action::Apply && fx::IsNativeCommand(a.command)) {  // acked by PollNative once the op really ran
                StartNative(pipe, a, now);
                continue;
            }
            const fx::EffectDef* d = a.kind == fx::Action::Apply ? fx::FindEffect(a.id) : nullptr;
            // At most once for traps (QA M4): the launcher marks it applied before the post, so a lost ack or a restart can never
            // repeat a trap. Good effects are acked after a successful post (at least once).
            if (d && d->trap) Ack(pipe, a.id, a.index);
            bool ok = PostHaloScript(a.command.c_str());
            if (!ok) {
                printf("[effects] POST FAILED %d (retry in %d ms): HS: %s\n", a.id, static_cast<int>(fx::kRetryMs), a.command.c_str());
                g_queue.Failed(a, now);
                continue;
            }
            if (a.kind != fx::Action::Reassert) {
                const char* what = a.kind == fx::Action::Apply ? "applied" : a.kind == fx::Action::Undo ? "undo" : "cleanup";
                printf("[effects] %s %d: HS: %s\n", what, a.id, a.command.c_str());
            }
            if (d && !d->trap) Ack(pipe, a.id, a.index);
        }
    }
}
