#pragma once

// Campaign difficulty, shared by the DLL (reads it) and the launcher (logs it). PLAN 5.5.
// CE: the game globals block (pointer in a RIP-relative global, halo1.dll RVA 0x2d91330) holds the difficulty as a
// short at +0x158 (encounter_place 0xbe0d54 switches on it: 0,1 normal count / 2 average / 3 insane count).
// Value names 0 easy, 1 normal, 2 heroic, 3 legendary (the three-way split in encounter_place only proves the
// 0/1, 2, 3 grouping, so the names are a hypothesis until a Heroic/Legendary log is seen).
// DLL -> launcher: the MISSION_LOAD message ends with " diff=<n>" (n < 0 or "-" = unreadable). Halo 2's observe
// lines already carry " diff=<n>" in their text, which the launcher parses the same way.

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace haloap {

    constexpr int kGlobalsDifficultyOffset = 0x158;  // CE game globals: short campaign difficulty

    // Plain read of the difficulty from a game globals block; -1 when the block is null. The caller wraps this in SEH.
    inline int DifficultyFromGlobals(const uint8_t* globals) {
        if (!globals) return -1;
        int16_t v;
        std::memcpy(&v, globals + kGlobalsDifficultyOffset, sizeof v);
        return v;
    }

    // "easy" / "normal" / "heroic" / "legendary", or nullptr for any other value.
    inline const char* DifficultyName(int d) {
        switch (d) {
        case 0: return "easy";
        case 1: return "normal";
        case 2: return "heroic";
        case 3: return "legendary";
        default: return nullptr;
        }
    }

    // The JSON value for a telemetry `difficulty` field: a quoted name, the raw number when the value is not 0..3,
    // and "" (leave the field out) when the value is unreadable (negative).
    inline std::string DifficultyJson(int d) {
        if (d < 0) return "";
        if (const char* n = DifficultyName(d)) return std::string("\"") + n + "\"";
        return std::to_string(d);
    }

    // The number in the LAST " diff=<digits>" token of `text` (a token ends at whitespace), -1 when absent, "-" or malformed.
    inline int ParseDiffToken(std::string_view text) {
        constexpr std::string_view key = " diff=";
        size_t pos = text.rfind(key);
        if (pos == std::string_view::npos) return -1;
        size_t i = pos + key.size();
        size_t start = i;
        int v = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9' && i - start < 6) v = v * 10 + (text[i++] - '0');
        if (i == start) return -1;
        if (i < text.size() && text[i] != ' ' && text[i] != '\t' && text[i] != '\r' && text[i] != '\n') return -1;
        return v;
    }

}  // namespace haloap
