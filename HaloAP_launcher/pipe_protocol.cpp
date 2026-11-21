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

    namespace {
        // Decimal (optionally '-'), or 0x-prefixed hex. Whole token must be consumed.
        bool ParseNum(std::string_view t, int64_t& out) {
            if (t.empty()) return false;
            bool neg = false;
            if (t.front() == '-') { neg = true; t.remove_prefix(1); }
            int base = 10;
            if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) {
                if (neg) return false;
                base = 16;
                t.remove_prefix(2);
            }
            if (t.empty()) return false;
            uint64_t v = 0;
            auto res = std::from_chars(t.data(), t.data() + t.size(), v, base);
            if (res.ec != std::errc() || res.ptr != t.data() + t.size()) return false;
            if (v > static_cast<uint64_t>(INT64_MAX)) return false;
            out = neg ? -static_cast<int64_t>(v) : static_cast<int64_t>(v);
            return true;
        }
    }

    std::optional<AiEvent> ParseAiEvent(std::string_view message) {
        AiEvent e{};
        std::string_view rest;
        if (auto r = AfterPrefix(message, "KILL: ")) { e.kill = true; rest = *r; }
        else if (auto r2 = AfterPrefix(message, "ERASE: ")) { e.kill = false; rest = *r2; }
        else return std::nullopt;
        rest = Trim(rest);

        constexpr int N = 13;
        static const std::string_view keys[N] = { "ah", "unit", "enc", "sq", "pl", "var", "fl", "ekf",
            "living", "total", "el", "et", "tid" };
        int64_t vals[N] = {};
        bool seen[N] = {};
        while (!rest.empty()) {
            size_t sp = rest.find(' ');
            std::string_view tok = rest.substr(0, sp);
            rest = (sp == std::string_view::npos) ? std::string_view() : rest.substr(sp + 1);
            if (tok.empty()) return std::nullopt;  // double space
            size_t eq = tok.find('=');
            if (eq == std::string_view::npos) return std::nullopt;
            std::string_view key = tok.substr(0, eq);
            int idx = -1;
            for (int i = 0; i < N; ++i) if (keys[i] == key) idx = i;
            if (idx < 0 || seen[idx]) return std::nullopt;
            if (!ParseNum(tok.substr(eq + 1), vals[idx])) return std::nullopt;
            seen[idx] = true;
        }
        for (bool s : seen) if (!s) return std::nullopt;

        auto idRange = [](int64_t v) { return v >= -1 && v <= 65535; };
        auto cntRange = [](int64_t v) { return v >= -1 && v <= 32767; };
        auto u32 = [](int64_t v) { return v >= 0 && v <= 0xFFFFFFFFll; };
        if (!u32(vals[0]) || !u32(vals[1])) return std::nullopt;
        if (!idRange(vals[2]) || !idRange(vals[3]) || !idRange(vals[4])) return std::nullopt;
        if (!u32(vals[5])) return std::nullopt;
        if (vals[6] < 0 || vals[6] > 0xFF || vals[7] < 0 || vals[7] > 0xFF) return std::nullopt;
        if (!cntRange(vals[8]) || !cntRange(vals[9]) || !cntRange(vals[10]) || !cntRange(vals[11])) return std::nullopt;
        if (!u32(vals[12])) return std::nullopt;
        e.ah = static_cast<uint32_t>(vals[0]);
        e.unit = static_cast<uint32_t>(vals[1]);
        e.enc = static_cast<int>(vals[2]);
        e.sq = static_cast<int>(vals[3]);
        e.pl = static_cast<int>(vals[4]);
        e.var = static_cast<uint32_t>(vals[5]);
        e.flags = static_cast<uint32_t>(vals[6]);
        e.ekf = static_cast<uint32_t>(vals[7]);
        e.living = static_cast<int>(vals[8]);
        e.total = static_cast<int>(vals[9]);
        e.el = static_cast<int>(vals[10]);
        e.et = static_cast<int>(vals[11]);
        e.tid = static_cast<uint32_t>(vals[12]);
        return e;
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
