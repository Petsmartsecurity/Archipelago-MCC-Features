#pragma once
#include "../pipe_client.h"

// Halo CE kill/erase events (ADR-003, docs/research/h1-kill-path.md).
// Hooks actor_died and actor_erase in halo1.dll and sends raw events to the launcher:
//   KILL: enc=<i> sq=<i> pl=<i> var=0x<8x> fl=0x<2x> living=<i> total=<i> tid=<u>
//   ERASE: (same fields)   -- actor_died reached from inside actor_erase (script/GC removal, not a kill)
//   SWARM: ...     -- (PLAN 5.7, log-only) a Flood infection form died; from a unit_death detour, installed only if its
//                     signature and globals check out (the KILL/ERASE hooks never depend on it). See shared/ai_event_format.h.
// The launcher maps events to checks; this code only reads game memory and queues a message.
namespace haloap {
    bool InstallKillHook(PipeClient* pipe);
    void UninstallKillHook();
}  // namespace haloap
