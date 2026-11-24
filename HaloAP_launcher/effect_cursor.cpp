#include "effect_cursor.h"
#include "pipe_protocol.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace haloap {

    std::string EffectFileName(const std::string& seed, const std::string& slot) {
        uint64_t h = 1469598103934665603ULL;  // FNV-1a 64 over "seed\nslot", as PendingFileName
        auto mix = [&h](unsigned char c) { h ^= c; h *= 1099511628211ULL; };
        for (unsigned char c : seed) mix(c);
        mix('\n');
        for (unsigned char c : slot) mix(c);
        char buf[48];
        std::snprintf(buf, sizeof(buf), "haloap_effects_%016llx.txt", static_cast<unsigned long long>(h));
        return buf;
    }

    std::string EffectMessage(int64_t id, int index) {
        return "EFFECT: " + std::to_string(id) + " " + std::to_string(index);
    }

    EffectCursor::EffectCursor(std::string path) : m_path(std::move(path)) { Load(); }

    void EffectCursor::Bind(const std::string& path) {
        if (path == m_path) return;
        m_cursor = -1;
        m_inFlight.clear();
        m_path = path;
        Load();
    }

    void EffectCursor::Load() {
        m_cursor = -1;
        m_inFlight.clear();
        if (m_path.empty()) return;
        std::ifstream f(m_path);
        if (!f.is_open()) return;
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::istringstream in(line);
            std::string tag;
            if (line.rfind("CURSOR ", 0) == 0) {
                long long c = -1;
                in >> tag >> c;
                if (in && c >= -1 && c <= 0x7FFFFFFF) m_cursor = static_cast<int>(c);
                continue;
            }
            long long idx = -1, id = 0;
            if ((in >> idx >> id) && idx >= 0 && idx <= 0x7FFFFFFF && IsEffectItemId(id)) m_inFlight[static_cast<int>(idx)] = id;
        }
        for (const auto& kv : m_inFlight) if (kv.first > m_cursor) m_cursor = kv.first;  // a torn file never re-opens an index
    }

    bool EffectCursor::OnItem(int64_t id, int index) {
        if (index < 0 || !IsEffectItemId(id) || index <= m_cursor) return false;
        m_cursor = index;
        m_inFlight[index] = id;
        Save();
        return true;
    }

    bool EffectCursor::OnDone(int64_t id, int index) {
        auto it = m_inFlight.find(index);
        if (it == m_inFlight.end() || it->second != id) return false;
        m_inFlight.erase(it);
        Save();
        return true;
    }

    std::vector<EffectItem> EffectCursor::InFlight() const {
        std::vector<EffectItem> out;
        for (const auto& kv : m_inFlight) out.push_back({ kv.first, kv.second });
        return out;
    }

    bool EffectCursor::Save() const {
        if (m_path.empty()) return true;
        const std::string tmp = m_path + ".tmp";
        bool ok = false;
        {
            std::ofstream f(tmp, std::ios::trunc);
            if (f.is_open()) {
                f << "CURSOR " << m_cursor << "\n";
                for (const auto& kv : m_inFlight) f << kv.first << " " << kv.second << "\n";
                f.flush();
                ok = static_cast<bool>(f);
            }
        }
        if (ok) {
            std::error_code ec;
            std::filesystem::rename(tmp, m_path, ec);
            if (ec) { std::filesystem::remove(tmp, ec); ok = false; }
        }
        return ok;
    }

}  // namespace haloap
