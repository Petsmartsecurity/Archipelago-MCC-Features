#include "pipe_protocol.h"

#include <charconv>
#include <system_error>

namespace haloap {

    namespace {
        bool IsWs(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

        std::string_view Trim(std::string_view s) {
            while (!s.empty() && IsWs(s.front())) s.remove_prefix(1);
            while (!s.empty() && IsWs(s.back())) s.remove_suffix(1);
            return s;
        }

        // Returns the text after `prefix`, or nullopt if message does not start with it.
        std::optional<std::string_view> AfterPrefix(std::string_view message, std::string_view prefix) {
            if (message.substr(0, prefix.size()) != prefix) return std::nullopt;
            return message.substr(prefix.size());
        }
    }

    std::optional<int64_t> ParsePositiveInt64(std::string_view text) {
        text = Trim(text);
        if (text.empty()) return std::nullopt;
        int64_t value = 0;
        const char* first = text.data();
        const char* last = text.data() + text.size();
        auto res = std::from_chars(first, last, value, 10);
        if (res.ec != std::errc() || res.ptr != last) return std::nullopt;
        if (value <= 0) return std::nullopt;
        return value;
    }

    std::optional<int64_t> ParseLocationChecked(std::string_view message) {
        auto rest = AfterPrefix(message, "LOCATION_CHECKED: ");
        if (!rest) return std::nullopt;
        return ParsePositiveInt64(*rest);
    }

    std::optional<std::string> ParseMissionComplete(std::string_view message) {
        auto rest = AfterPrefix(message, "MISSION_COMPLETE: ");
        if (!rest) return std::nullopt;
        std::string_view code = Trim(*rest);
        if (code.empty() || code.size() > 32) return std::nullopt;
        for (char c : code) {
            if (c <= ' ' || c >= 0x7f) return std::nullopt;
        }
        return std::string(code);
    }

    std::optional<std::string> ParseMissionLoad(std::string_view message) {
        auto rest = AfterPrefix(message, "MISSION_LOAD: ");
        if (!rest) return std::nullopt;
        constexpr std::string_view marker = "levels\\";
        size_t pos = rest->find(marker);
        if (pos == std::string_view::npos) return std::nullopt;
        size_t start = pos + marker.size();
        size_t end = rest->find('\\', start);
        if (end == std::string_view::npos || end == start) return std::nullopt;
        return std::string(rest->substr(start, end - start));
    }

    std::optional<int> ParseChapter(std::string_view message) {
        auto rest = AfterPrefix(message, "CHAPTER:");
        if (!rest) return std::nullopt;
        std::string_view t = Trim(*rest);
        if (t.empty()) return std::nullopt;
        int value = 0;
        auto res = std::from_chars(t.data(), t.data() + t.size(), value, 10);
        if (res.ec != std::errc() || res.ptr != t.data() + t.size()) return std::nullopt;
        if (value < 0 || value > 999) return std::nullopt;
        return value;
    }

}  // namespace haloap
