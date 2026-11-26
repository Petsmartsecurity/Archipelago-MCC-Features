#pragma once
// The "observe_logging" switch (Halo 2 / Halo 3 observe-only telemetry hooks), shared by the launcher and the DLL:
//   haloap_config.txt (next to the launcher): line 1 = MCC game root, further lines `key=value`; `observe_logging=0` turns the
//   Halo 2 / Halo 3 observers off (default: on). The launcher sends "OBSERVE: 0|1" to the DLL when the pipe connects.
// Header-only and pure so mcc-ap/tests/test_module_observer.cpp can run it.
#include <cctype>
#include <cstring>
#include <string>

namespace haloap {

    constexpr const char* kObserveMessagePrefix = "OBSERVE: ";
    constexpr const char* kObserveConfigKey = "observe_logging";

    // Launcher -> DLL line.
    inline std::string ObserveMessage(bool on) { return std::string(kObserveMessagePrefix) + (on ? "1" : "0"); }

    // DLL side: exactly "OBSERVE: 0" or "OBSERVE: 1". Anything else is rejected (returns false, *on untouched).
    inline bool ParseObserveMessage(const char* msg, bool* on) {
        const size_t n = std::strlen(kObserveMessagePrefix);
        if (!msg || std::strncmp(msg, kObserveMessagePrefix, n) != 0) return false;
        if ((msg[n] != '0' && msg[n] != '1') || msg[n + 1] != 0) return false;
        *on = msg[n] == '1';
        return true;
    }

    // One value of the config file: 0/1/off/on/false/true/no/yes (case-insensitive, surrounding blanks ignored).
    // Unknown text keeps the default (on): a typo must not silently change what friends log.
    inline bool ParseBoolWord(const std::string& raw, bool defaultValue) {
        size_t b = 0, e = raw.size();
        while (b < e && std::isspace(static_cast<unsigned char>(raw[b]))) b++;
        while (e > b && std::isspace(static_cast<unsigned char>(raw[e - 1]))) e--;
        std::string v;
        for (size_t i = b; i < e; i++) v += static_cast<char>(std::tolower(static_cast<unsigned char>(raw[i])));
        if (v == "0" || v == "off" || v == "false" || v == "no") return false;
        if (v == "1" || v == "on" || v == "true" || v == "yes") return true;
        return defaultValue;
    }

    // Whole file text -> observe_logging. Line 1 is the game directory and is never read as a setting. Last line wins.
    inline bool ParseObserveLogging(const std::string& fileText) {
        bool on = true;
        size_t pos = 0;
        bool first = true;
        const std::string key = kObserveConfigKey;
        while (pos <= fileText.size()) {
            size_t nl = fileText.find('\n', pos);
            if (nl == std::string::npos) nl = fileText.size();
            std::string line = fileText.substr(pos, nl - pos);
            pos = nl + 1;
            if (first) { first = false; continue; }
            while (!line.empty() && (line.back() == '\r' || std::isspace(static_cast<unsigned char>(line.back())))) line.pop_back();
            size_t b = 0;
            while (b < line.size() && std::isspace(static_cast<unsigned char>(line[b]))) b++;
            line = line.substr(b);
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = line.substr(0, eq);
            while (!k.empty() && std::isspace(static_cast<unsigned char>(k.back()))) k.pop_back();
            if (k == key) on = ParseBoolWord(line.substr(eq + 1), true);
        }
        return on;
    }

    // First line of the file without its line terminator (the game directory; a Notepad CRLF must not end up in a path).
    inline std::string ConfigGameDir(const std::string& fileText) {
        std::string line = fileText.substr(0, fileText.find('\n'));
        while (!line.empty() && line.back() == '\r') line.pop_back();
        return line;
    }

    // New file text with line 1 replaced by `dir` and every other line kept (SaveGameDir must not drop settings).
    inline std::string ReplaceConfigGameDir(const std::string& fileText, const std::string& dir) {
        const size_t nl = fileText.find('\n');
        if (nl == std::string::npos) return dir;
        return dir + fileText.substr(nl);
    }

}  // namespace haloap
