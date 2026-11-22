#pragma once
#include "../pipe_client.h"

// Halo 2 OBSERVE-ONLY hook set (ADR-004 decision 5; docs/stage-reports/phase8-h2-7b-dll.md).
// Logs mission load / complete / chapter title / shell messages / actor deaths from halo2.dll as pipe messages
//   H2: ev=<name> n=<seq> tid=<thread> k=v ...
// and changes nothing in the game. Each hook is independent: a missing or ambiguous signature turns off that hook only.
namespace haloap {
    // Called by the worker loop every ~100 ms. Cheap (GetModuleHandle compare) except when halo2.dll appears,
    // changes or every ~2 s (identity + hook-prologue check).
    void H2Tick(PipeClient* pipe);
    // Removes the hooks (when halo2.dll is still the module we hooked). Call before the pipe is freed.
    void UninstallH2Observe();
}  // namespace haloap
