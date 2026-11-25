#pragma once
#include "../pipe_client.h"

// Halo 3 OBSERVE-ONLY hook set (PLAN 8.H3.2; docs/stage-reports/phase8-h3-2-observe.md).
// Logs mission load / map reset / revert / restart / quit / game won / chapter title / shell messages / actor deaths and erases
// from halo3.dll as pipe messages
//   H3: ev=<name> n=<seq> tid=<thread> k=v ...
// and changes nothing in the game. Hooks exist only while halo3.dll is loaded (installed on load, removed on unload).
// Each hook is independent: a missing or ambiguous signature turns off that hook only.
namespace haloap {
    // Called by the worker loop every ~100 ms. Cheap (GetModuleHandle compare) except when halo3.dll appears,
    // changes or every ~2 s (identity + hook-prologue check).
    void H3Tick(PipeClient* pipe);
    // True while halo3.dll is the loaded game module the observer is attached to or about to attach to.
    bool H3Loaded();
    // Removes the hooks (when halo3.dll is still the module we hooked). Call before the pipe is freed.
    void UninstallH3Observe();
}  // namespace haloap
