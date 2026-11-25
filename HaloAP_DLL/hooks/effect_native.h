#pragma once
#include "effect_native_logic.h"

namespace haloap {
    // CE effect items, phase B (ADR-005 "Phase B"): native engine calls run inside a detour of the evaluate function of
    // `cheat_spawn_warthog` (the "gateway"). The worker thread fills a request slot and posts "(cheat_spawn_warthog)" through the
    // normal HS channel; the engine runs that line synchronously on its own console path, our detour executes the op, always pops
    // the HS stack (calls 0xacbfac, resolved from the evaluate's own tail jump) and never calls the original (so no warthog).
    // Every address comes from FindUniquePatternInModule; a missing pattern disables the items that need it, and a failed gateway
    // disables all native items (they are then answered EFFECT_SKIPPED by effects.cpp).

    // The HS line that triggers the gateway (no arguments, so the scenario `references` rule never applies).
    constexpr const char* kGatewayCommand = "(cheat_spawn_warthog)";

    // halo1.dll loaded: resolve natives, verify and hook the gateway. False = nothing native is available.
    bool InstallNativeEffects();
    void UninstallNativeEffects();

    // True once an install pass ran for the current halo1.dll (whatever its outcome); false before it and after an uninstall.
    // effects.cpp holds a native item (retry) until then and skips it for good only after a pass left its op unavailable.
    bool NativeInstallAttempted();

    // True when the gateway is installed and every native this op needs resolved.
    bool NativeOpAvailable(fx::Op op);

    // The slot shared with the detour (worker thread side: Request / Take / Cancel).
    fx::OpSlot& NativeSlot();

    // Name of the weapon tag the last drop created (empty before the first drop); the future on-screen notice names it.
    const char* NativeLastDropName();
}
