#include "telemetry_log.h"

#include <cstdio>
#include <ctime>
#include <exception>

namespace haloap {

    std::string JsonString(const std::string& s) {
        static const char* hex = "0123456789abcdef";
        std::string out = "\"";
        for (char ch : s) {
            unsigned char c = static_cast<unsigned char>(ch);
            switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out += hex[c >> 4];
                    out += hex[c & 15];
                } else {
                    out += ch;
                }
            }
        }
        out += '"';
        return out;
    }

    namespace {
        std::string HexU32(uint32_t v, int digits) {
            char buf[16];
            std::snprintf(buf, sizeof buf, "0x%0*x", digits, static_cast<unsigned>(v));
            return buf;
        }
    }

    TelemetryLog::TelemetryLog(std::string dir, std::string build, std::string launcherVersion)
        : m_dir(std::move(dir)), m_build(std::move(build)), m_version(std::move(launcherVersion)),
          m_start(std::chrono::steady_clock::now()) {}

    std::string TelemetryLog::Path() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_path;
    }

    void TelemetryLog::WriteLine(const std::string& ev, const std::string& fieldsAfterT) {
        // Caller holds m_mutex.
        try {
            if (m_failed) return;
            if (!m_file.is_open()) {
                std::time_t now = std::time(nullptr);
                std::tm tmv{};
#ifdef _WIN32
                localtime_s(&tmv, &now);
#else
                localtime_r(&now, &tmv);
#endif
                char stamp[32];
                std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tmv);
                m_path = m_dir + "haloap_telemetry_" + stamp + ".jsonl";
                m_file.open(m_path, std::ios::app | std::ios::binary);
                if (!m_file.is_open()) {
                    m_failed = true;
                    std::fprintf(stderr, "[telemetry] cannot open %s; telemetry disabled\n", m_path.c_str());
                    m_path.clear();
                    return;
                }
            }
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - m_start).count();
            if (!m_sessionWritten) {
                m_sessionWritten = true;
                m_file << "{\"ev\":\"session\",\"t\":" << ms << ",\"gen\":0"
                    << ",\"build\":" << JsonString(m_build) << ",\"launcher\":" << JsonString(m_version) << "}\n";
            }
            m_file << "{\"ev\":" << JsonString(ev) << ",\"t\":" << ms << ",\"gen\":" << m_gen
                << fieldsAfterT << "}\n";
            m_file.flush();
            if (!m_file) {
                m_failed = true;
                std::fprintf(stderr, "[telemetry] write failed; telemetry disabled\n");
            }
        } catch (const std::exception&) {
            m_failed = true;
            std::fprintf(stderr, "[telemetry] internal error; telemetry disabled\n");
        } catch (...) {
            m_failed = true;
        }
    }

    void TelemetryLog::MissionLoad(const std::string& mission) {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_gen;
        m_rejected = 0;
        WriteLine("mission_load", ",\"mission\":" + JsonString(mission));
    }

    void TelemetryLog::Pipe(bool connected) {
        std::lock_guard<std::mutex> lock(m_mutex);
        WriteLine("pipe", std::string(",\"state\":") + (connected ? "\"connected\"" : "\"disconnected\""));
    }

    void TelemetryLog::Rejected() {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_rejected;
    }

    void TelemetryLog::MissionEnd(const std::string& mission, const std::string& reason) {
        std::lock_guard<std::mutex> lock(m_mutex);
        WriteLine("mission_end", ",\"mission\":" + JsonString(mission) + ",\"reason\":" + JsonString(reason)
            + ",\"rejected\":" + std::to_string(m_rejected));
    }

    void TelemetryLog::Event(const AiEvent& e, const char* ev, const std::string& mission) {
        std::lock_guard<std::mutex> lock(m_mutex);
        const std::string m = JsonString(mission);
        WriteLine(ev, ",\"mission\":" + m + ",\"enc\":" + std::to_string(e.enc) + ",\"sq\":" + std::to_string(e.sq)
            + ",\"pl\":" + std::to_string(e.pl) + ",\"variant\":" + std::to_string(e.var & 0xFFFFu)
            + ",\"swarm\":" + ((e.flags & 1u) ? "true" : "false")
            + ",\"var_handle\":" + JsonString(HexU32(e.var, 8)) + ",\"ah\":" + JsonString(HexU32(e.ah, 8))
            + ",\"unit\":" + JsonString(HexU32(e.unit, 8)) + ",\"ekf\":" + JsonString(HexU32(e.ekf, 2))
            + ",\"el\":" + std::to_string(e.el) + ",\"et\":" + std::to_string(e.et)
            + ",\"tid\":" + std::to_string(e.tid));
        WriteLine("squad", ",\"mission\":" + m + ",\"enc\":" + std::to_string(e.enc) + ",\"sq\":" + std::to_string(e.sq)
            + ",\"living\":" + std::to_string(e.living) + ",\"spawned\":" + std::to_string(e.total));
    }

    void TelemetryLog::Death(const std::string& mission, const AiEvent& e) { Event(e, "death", mission); }
    void TelemetryLog::Erase(const std::string& mission, const AiEvent& e) { Event(e, "erase", mission); }

}  // namespace haloap
