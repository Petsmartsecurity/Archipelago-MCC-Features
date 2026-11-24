#pragma once

// KILL/ERASE pipe message sent by the DLL (kill_hook) and parsed by the launcher (ParseAiEvent).
// Use with snprintf: first argument is "KILL" or "ERASE". Both sides share this string so they cannot drift apart.
//   ah   actor handle          unit  unit object handle      enc/sq/pl  encounter/squad/platoon (-1 = none)
//   var  variant tag handle    fl    actor flags (+6)         ekf  unit kill flags (obj+0xF6 & 0x60)
//   living/total  squad counters after the death   el/et  encounter living/total after the death
//   tid  thread id. Any int may be -1 if unreadable; handles may be 0xffffffff.
//
// SWARM pipe message (log-only, PLAN 5.7): a Flood infection form (swarm member) died. Parsed by ParseSwarmEvent, which
// no KILL/ERASE consumer ever sees. Fields: ah swarm actor handle, unit the dying unit's handle, enc/sq encounter/squad of
// the swarm actor, var its variant tag handle, ekf unit kill flags (obj+0xF6 & 0x60), members swarm member count BEFORE this
// death (actor +0x14), tid thread id. Any int may be -1 if unreadable.
namespace haloap {
    constexpr const char* kAiEventFormat =
        "%s: ah=0x%08x unit=0x%08x enc=%d sq=%d pl=%d var=0x%08x fl=0x%02x ekf=0x%02x "
        "living=%d total=%d el=%d et=%d tid=%lu";
}

namespace haloap {
    constexpr const char* kSwarmEventFormat =
        "SWARM: ah=0x%08x unit=0x%08x enc=%d sq=%d var=0x%08x ekf=0x%02x members=%d tid=%lu";
}
