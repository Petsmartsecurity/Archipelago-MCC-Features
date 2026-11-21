#pragma once

// KILL/ERASE pipe message sent by the DLL (kill_hook) and parsed by the launcher (ParseAiEvent).
// Use with snprintf: first argument is "KILL" or "ERASE". Both sides share this string so they cannot drift apart.
//   ah   actor handle          unit  unit object handle      enc/sq/pl  encounter/squad/platoon (-1 = none)
//   var  variant tag handle    fl    actor flags (+6)         ekf  unit kill flags (obj+0xF6 & 0x60)
//   living/total  squad counters after the death   el/et  encounter living/total after the death
//   tid  thread id. Any int may be -1 if unreadable; handles may be 0xffffffff.
namespace haloap {
    constexpr const char* kAiEventFormat =
        "%s: ah=0x%08x unit=0x%08x enc=%d sq=%d pl=%d var=0x%08x fl=0x%02x ekf=0x%02x "
        "living=%d total=%d el=%d et=%d tid=%lu";
}
