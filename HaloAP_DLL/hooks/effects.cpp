#include "effects.h"
#include "effect_queue.h"
#include "shell_level_load.h"
#include <windows.h>
#include <cstdio>
#include <mutex>

namespace haloap {
    namespace {
        std::mutex g_mutex;
        fx::EffectQueue g_queue;
        bool g_inMission = false;
        bool g_loadPending = false;  // a mission load happened since the last tick: restart the queue's settle clock

        void Skipped(PipeClient* pipe, int id, long index, const char* why) {
            printf("[effects] skipped %d (index %ld): %s\n", id, index, why);
            if (pipe && pipe->IsConnected()) pipe->SendAsync("EFFECT_SKIPPED: " + std::to_string(id) + " " + std::to_string(index));
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
        fx::Enqueue r;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            r = g_queue.Add(id, index);
        }
        switch (r) {
        case fx::Enqueue::Queued: printf("[effects] queued %d (index %ld)\n", id, index); break;
        case fx::Enqueue::Duplicate: break;  // the launcher re-sends unconfirmed items on every connect
        case fx::Enqueue::UnknownId: Skipped(pipe, id, index, "unknown effect id (DLL older than the seed?)"); break;
        case fx::Enqueue::Full: Skipped(pipe, id, index, "queue full"); break;
        }
    }

    void EffectsTick(PipeClient* pipe) {
        const uint64_t now = GetTickCount64();
        std::vector<fx::Action> actions;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_loadPending) { g_queue.OnMissionLoad(now); g_loadPending = false; }
            fx::Gate gate;
            gate.inMission = g_inMission;
            gate.engineReady = CanPostHaloScript();
            actions = g_queue.Tick(now, gate);
        }
        for (const auto& a : actions) {
            bool ok = PostHaloScript(a.command.c_str());
            const char* what = a.kind == fx::Action::Apply ? "applied" : a.kind == fx::Action::Undo ? "undo" : "cleanup";
            printf("[effects] %s %d: HS: %s%s\n", what, a.id, a.command.c_str(), ok ? "" : "  (POST FAILED)");
            if (a.kind == fx::Action::Apply && pipe && pipe->IsConnected())
                pipe->SendAsync("EFFECT_APPLIED: " + std::to_string(a.id) + " " + std::to_string(a.index));
        }
    }
}
