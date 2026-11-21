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

    // "MISSION_LOAD: ...levels\<code>\..." -> code (non-empty path component after "levels\").
    std::optional<std::string> ParseMissionLoad(std::string_view message);

    // "CHAPTER:<n>" -> n in [0, 999].
    std::optional<int> ParseChapter(std::string_view message);

}  // namespace haloap
