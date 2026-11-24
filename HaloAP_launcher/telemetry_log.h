#pragma once
// Appends one JSON object per line to haloap_telemetry_<yyyymmdd-hhmmss>.jsonl (format: docs/telemetry-format.md).
// Pure C++, thread-safe (internal mutex), never throws. The file is created lazily on the first write.

#include <chrono>
#include <fstream>
#include <mutex>
#include <string>

#include "pipe_protocol.h"
#include "shared/common.h"

namespace haloap {

    class TelemetryLog {
    public:
        // `dir` should end with a path separator (or be empty for the working directory).
        // The `session` line (build tag + launcher version) is written once, first, when the file is created.
        explicit TelemetryLog(std::string dir, std::string build = "", std::string launcherVersion = haloap::kVersion);

        void MissionLoad(const std::string& mission);  // increments `gen`, resets the rejected counter
        void Pipe(bool connected);
        void Rejected();  // a KILL/ERASE failed to parse; reported as "rejected" on mission_end
        void MissionEnd(const std::string& mission, const std::string& reason);
        // Writes the death line, then the squad line.
        void Death(const std::string& mission, const AiEvent& e);
        // Writes the erase line, then the squad line.
        void Erase(const std::string& mission, const AiEvent& e);

        // Observe-only Halo 2 line (DLL message "H2: <text>"): {"ev":"h2",...,"msg":"<text>"}. Text is cut to
        // kH2MsgMax bytes at a character boundary and invalid UTF-8 becomes U+FFFD. Does not change `gen`.
        static constexpr size_t kH2MsgMax = 2048;
        void H2(const std::string& msg);

        // Log-only Flood infection-form death (DLL message "SWARM: ..."): {"ev":"swarm",...}. No squad line, no effect
        // on anything else (PLAN 5.7).
        void Swarm(const std::string& mission, const SwarmEvent& e);

        // An effect item the DLL applied (EFFECT_APPLIED) or could never apply (EFFECT_SKIPPED): {"ev":"effect",...}. Does not
        // change `gen`.
        void Effect(const std::string& mission, int64_t id, int index, bool applied);

        // Path of the open file, empty until the first write succeeded.
        std::string Path();

    private:
        void Event(const AiEvent& e, const char* ev, const std::string& mission);
        void WriteLine(const std::string& ev, const std::string& fieldsAfterT);  // fieldsAfterT: e.g. ,"mission":"a10"
        std::string m_dir;
        std::string m_build;
        std::string m_version;
        bool m_sessionWritten{ false };
        int m_gen{ 0 };
        int m_rejected{ 0 };
        std::string m_path;
        std::ofstream m_file;
        bool m_failed{ false };
        std::chrono::steady_clock::time_point m_start;
        std::mutex m_mutex;
    };

    // JSON string literal including the quotes (escapes ", \, control chars; non-ASCII bytes pass through).
    std::string JsonString(const std::string& s);

}  // namespace haloap
