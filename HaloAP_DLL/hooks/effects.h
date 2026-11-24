#pragma once
#include "../pipe_client.h"

namespace haloap {
    // CE effect items (ADR-005, phase A: HaloScript string channel only, no new hook).
    //
    // PostHaloScript (shell_level_load.cpp) posts "HS: <cmd>" through the engine object's vtable slot 9 ORIGINAL function.
    // Thread: the push is a lock-free SList push and the string is evaluated by the engine's own frame pump
    // (docs/research/item-effects.md 1.1, "thread" row), so the worker thread may post.

    // Pipe thread: "EFFECT: <id> <index>" from the launcher. Queues it (duplicates are ignored) or answers
    // "EFFECT_SKIPPED: <id> <index>" when it can never apply (unknown id / queue full) so the launcher stops re-sending it.
    void EffectsOnMessage(PipeClient* pipe, const std::string& message);

    // Worker thread, every 100 ms: applies what the gate allows, expires timed effects.
    void EffectsTick(PipeClient* pipe);

    // Mission state as the effects see it: true when a mission load starts (also restarts the settle clock), false at
    // teardown / mission complete / quit / halo1 unload. The skull hook's flag is not used: shell message 0x1 clears it
    // and it is unknown whether MCC also sends 0x1 on a plain unpause.
    void EffectsSetInMission(bool inMission);
}
