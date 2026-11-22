#include "pattern_scan.h"
#include <cctype>
#include <cstdio>
#include <cstring>

namespace haloap {

    ParsedPattern ParsePattern(const std::string& pattern) {
        ParsedPattern result;
        size_t i = 0;
        while (i < pattern.size()) {
            // Skip whitespace.
            while (i < pattern.size() && std::isspace((unsigned char)pattern[i])) i++;
            if (i >= pattern.size()) break;

            if (pattern[i] == '?') {
                // Wildcard. Accept either "?" or "??".
                result.bytes.push_back(0);
                result.mask.push_back(false);
                i++;
                if (i < pattern.size() && pattern[i] == '?') i++;
            }
            else {
                // Hex byte.
                char hex[3] = { pattern[i], 0, 0 };
                i++;
                if (i < pattern.size() && std::isxdigit((unsigned char)pattern[i])) {
                    hex[1] = pattern[i];
                    i++;
                }
                result.bytes.push_back((uint8_t)std::strtoul(hex, nullptr, 16));
                result.mask.push_back(true);
            }
        }
        return result;
    }

    void* FindPatternInModule(HMODULE module, const std::string& pattern) {
        if (!module) return nullptr;

        ParsedPattern parsed = ParsePattern(pattern);
        if (parsed.bytes.empty()) return nullptr;

        // Get the module's memory range from its PE headers.
        auto dosHeader = (IMAGE_DOS_HEADER*)module;
        if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;

        auto ntHeaders = (IMAGE_NT_HEADERS*)((uint8_t*)module + dosHeader->e_lfanew);
        if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) return nullptr;

        uint8_t* base = (uint8_t*)module;
        size_t size = ntHeaders->OptionalHeader.SizeOfImage;

        // Linear scan. Could be faster with Boyer-Moore but this is fine for now.
        size_t patLen = parsed.bytes.size();
        if (size < patLen) return nullptr;

        size_t end = size - patLen;
        for (size_t i = 0; i <= end; i++) {
            bool match = true;
            for (size_t j = 0; j < patLen; j++) {
                if (parsed.mask[j] && base[i + j] != parsed.bytes[j]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                return base + i;
            }
        }

        return nullptr;
    }

    size_t CountMatches(const uint8_t* data, size_t len, const ParsedPattern& pat, size_t maxHits, size_t* firstOffset) {
        const size_t n = pat.bytes.size();
        if (!data || n == 0 || len < n || maxHits == 0) return 0;
        // vector<bool> is slow in a hot loop: compact the mask to bytes once.
        std::vector<uint8_t> m(n);
        size_t anchor = n;
        for (size_t i = 0; i < n; i++) {
            m[i] = pat.mask[i] ? 1 : 0;
            if (m[i] && anchor == n) anchor = i;
        }
        if (anchor == n) return 0;
        const uint8_t first = pat.bytes[anchor];
        const uint8_t* p = data + anchor;
        const uint8_t* end = data + (len - n) + anchor + 1;  // one past the last anchor position
        size_t hits = 0;
        while (p < end) {
            p = static_cast<const uint8_t*>(memchr(p, first, static_cast<size_t>(end - p)));
            if (!p) break;
            const size_t start = static_cast<size_t>(p - data) - anchor;
            bool ok = true;
            for (size_t j = 0; j < n; j++) {
                if (m[j] && data[start + j] != pat.bytes[j]) { ok = false; break; }
            }
            if (ok) {
                if (hits == 0 && firstOffset) *firstOffset = start;
                if (++hits >= maxHits) break;
            }
            ++p;
        }
        return hits;
    }

    namespace {
        // __try needs a function without C++ objects needing unwind: plain pointers only.
        size_t SafeCount(const uint8_t* data, size_t len, const ParsedPattern* pat, size_t maxHits, size_t* first) {
            __try {
                return CountMatches(data, len, *pat, maxHits, first);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                return maxHits;  // unreadable: report "ambiguous" so the caller fails closed
            }
        }
    }

    void* FindUniquePatternInModule(HMODULE module, const std::string& pattern, int* outHits) {
        if (outHits) *outHits = 0;
        if (!module) return nullptr;
        ParsedPattern parsed = ParsePattern(pattern);
        if (parsed.bytes.empty()) return nullptr;

        auto dos = (IMAGE_DOS_HEADER*)module;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
        auto nt = (IMAGE_NT_HEADERS*)((uint8_t*)module + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
        uint8_t* base = (uint8_t*)module;
        const size_t imageSize = nt->OptionalHeader.SizeOfImage;

        size_t hits = 0;
        uint8_t* found = nullptr;
        IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections && hits < 2; i++, sec++) {
            if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            size_t va = sec->VirtualAddress;
            size_t size = sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData;
            if (va >= imageSize) continue;
            if (size > imageSize - va) size = imageSize - va;
            size_t first = 0;
            size_t n = SafeCount(base + va, size, &parsed, 2 - hits, &first);
            if (n && hits == 0) found = base + va + first;
            hits += n;
        }
        if (outHits) *outHits = hits >= 2 ? 2 : static_cast<int>(hits);
        return hits == 1 ? found : nullptr;
    }

}  // namespace haloap