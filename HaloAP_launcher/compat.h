#pragma once
// Pure helpers, no dependencies: server address normalisation and seed/launcher compatibility (PLAN 6.R QA B1, M2).
#include <string>

namespace haloap {

    // The version this launcher sends in the AP `Connect` packet. The apworld's required_client_version is the same
    // number (halo_mcc/__init__.py), so a launcher that sends less (upstream MCC-AP sends 1.2.0) is refused by the
    // server with IncompatibleVersion (MultiServer.py: `minver > args['version']`).
    constexpr int kClientVersionMajor = 1;
    constexpr int kClientVersionMinor = 2;
    constexpr int kClientVersionBuild = 10;

    // The newest slot_data "killsanity_version" this launcher understands (apworld fill_slot_data).
    constexpr int kKillsanityVersion = 1;

    // What the user typed -> the address handed to APClient.
    //  - trimmed;
    //  - with a scheme (ws:// or wss://): kept as is;
    //  - loopback (localhost, 127.0.0.1, [::1]) without a scheme: ws:// (a local server has no TLS);
    //  - anything else without a scheme: unchanged, so apclientpp itself tries wss:// first and falls back to ws://
    //    (apclient.hpp: `_tryWSS` is set only for a scheme-less URI, onerror() flips between the two, and a default
    //    port 38281 is added). Forcing wss:// here would turn that fallback off.
    std::string NormalizeServerAddress(const std::string& typed);

    enum class SeedCompat { Ok, Missing, Newer };
    // `version` = slot_data["killsanity_version"], or -1 when the key is absent / not an integer.
    SeedCompat CheckSeedCompat(int version);

}  // namespace haloap
