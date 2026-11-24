#pragma once
// Pure, dependency-free parsers for DLL -> launcher pipe messages.
// None of these can throw: malformed input yields std::nullopt.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace haloap {

    // Parses a base-10 integer in (0, INT64_MAX]. Surrounding ASCII whitespace is trimmed;
    // empty text, '+', '-', trailing junk, zero, negatives and overflow all yield nullopt.
    std::optional<int64_t> ParsePositiveInt64(std::string_view text);

    // "LOCATION_CHECKED: <id>" -> id (> 0).
    std::optional<int64_t> ParseLocationChecked(std::string_view message);

    // "MISSION_COMPLETE: <code>" -> code (non-empty, printable, no spaces, <= 32 chars).
    std::optional<std::string> ParseMissionComplete(std::string_view message);

    // "MISSION_LOAD: ...levels\<code>\..." (CE) or "...scenarios\solo\<code>\..." (Halo 2) -> code (non-empty path
    // component after the marker).
    std::optional<std::string> ParseMissionLoad(std::string_view message);

    // "CHAPTER:<n>" -> n in [0, 999].
    std::optional<int> ParseChapter(std::string_view message);

    // "KILL: ah=0x<hex> unit=0x<hex> enc=<int> sq=<int> pl=<int> var=0x<hex> fl=0x<hex> ekf=0x<hex> living=<int>
    //  total=<int> el=<int> et=<int> tid=<uint>" (or "ERASE: ..."); see shared/ai_event_format.h.
    // All keys required exactly once, in any order.
    struct AiEvent {
        bool kill;  // true = KILL, false = ERASE
        uint32_t ah, unit;  // actor / unit handles (0xffffffff possible)
        int enc, sq, pl;  // -1 = none/unknown, else 0..65535
        uint32_t var;
        uint32_t flags;  // actor flags byte
        uint32_t ekf;  // unit kill flags (obj+0xF6 & 0x60), 0..0xFF
        int living, total;  // squad counters: -1 = unreadable, else 0..32767
        int el, et;  // encounter living/total: -1 = unreadable, else 0..32767
        uint32_t tid;
    };
    std::optional<AiEvent> ParseAiEvent(std::string_view message);

    // "SWARM: ah=0x<hex> unit=0x<hex> enc=<int> sq=<int> var=0x<hex> ekf=0x<hex> members=<int> tid=<uint>": a Flood
    // infection form died (PLAN 5.7, log-only). See shared/ai_event_format.h. All keys required exactly once, any order.
    struct SwarmEvent {
        uint32_t ah, unit, var, ekf, tid;
        int enc, sq;   // -1 = none/unreadable, else 0..65535
        int members;   // swarm member count before the death: -1 = unreadable, else 0..32767
    };
    std::optional<SwarmEvent> ParseSwarmEvent(std::string_view message);

}  // namespace haloap
