#pragma once
#pragma once

#include <windows.h>
#include <vector>
#include <string>

namespace haloap {

    // Parse a pattern string like "48 89 5C 24 08 ? ? ?? 48 81 EC"
    // where "?" or "??" are wildcards.
    // Returns (bytes, mask) where mask[i] = 1 if bytes[i] must match, 0 if wildcard.
    struct ParsedPattern {
        std::vector<uint8_t> bytes;
        std::vector<bool> mask;
    };
    ParsedPattern ParsePattern(const std::string& pattern);

    // Find a pattern in a module's loaded memory.
    // Returns the address of the first match, or nullptr if not found.
    void* FindPatternInModule(HMODULE module, const std::string& pattern);

    // Counts matches in a raw buffer, stopping at maxHits (memchr on the first concrete byte; a pattern with no
    // concrete byte matches nothing). *firstOffset = offset of the first match when hits > 0.
    size_t CountMatches(const uint8_t* data, size_t len, const ParsedPattern& pat, size_t maxHits, size_t* firstOffset);

    // Scans the EXECUTABLE sections only and returns the match address iff there is exactly one match in the module.
    // *outHits (optional) = 0 (none), 1, or 2 (two or more). The module must stay mapped during the call (pin it).
    void* FindUniquePatternInModule(HMODULE module, const std::string& pattern, int* outHits = nullptr);

}