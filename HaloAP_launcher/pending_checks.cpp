#include "pending_checks.h"
#include "pipe_protocol.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace haloap {

    std::string PendingFileName(const std::string& seed, const std::string& slot) {
        // FNV-1a 64 over "seed\nslot"
        uint64_t h = 1469598103934665603ULL;
        auto mix = [&h](unsigned char c) { h ^= c; h *= 1099511628211ULL; };
        for (unsigned char c : seed) mix(c);
        mix('\n');
        for (unsigned char c : slot) mix(c);
        char buf[48];
        std::snprintf(buf, sizeof(buf), "haloap_pending_%016llx.txt", static_cast<unsigned long long>(h));
        return buf;
    }

    PendingChecks::PendingChecks(std::string path) : m_path(std::move(path)) {}

    void PendingChecks::Bind(const std::string& path) {
        if (!m_path.empty() && path != m_path) {
            // Reconnected to a different seed in the same run: what's in memory belongs to (and is saved in) the old
            // seed's file. Its server confirmations would also wrongly block the same IDs in the new seed.
            m_pending.clear();
            m_confirmed.clear();
            m_goals.clear();
        }
        std::set<int64_t> queued = m_pending;  // queued before the seed was known
        std::set<std::string> goals = m_goals;
        m_path = path;
        Load();
        bool changed = false;
        for (const auto& g : goals) changed = m_goals.insert(g).second || changed;
        for (int64_t id : queued) {
            if (!m_confirmed.count(id) && m_pending.insert(id).second) changed = true;
        }
        if (changed) Save();
    }

    void PendingChecks::Load() {
        m_pending.clear();
        m_goals.clear();
        if (m_path.empty()) return;
        std::ifstream f(m_path);
        if (!f.is_open()) return;
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line == "GOAL") { m_goals.insert("CE"); continue; }  // written before per-game flags
            if (line.rfind("GOAL:", 0) == 0) { if (line.size() > 5 && line.size() <= 13) m_goals.insert(line.substr(5)); continue; }
            if (auto id = ParsePositiveInt64(line)) m_pending.insert(*id);
        }
    }

    bool PendingChecks::Add(int64_t id) {
        if (id <= 0 || m_confirmed.count(id) || m_pending.count(id)) return false;
        m_pending.insert(id);
        Save();
        return true;
    }

    bool PendingChecks::Confirm(int64_t id) {
        m_confirmed.insert(id);
        bool wasPending = m_pending.erase(id) != 0;
        if (wasPending) Save();
        return wasPending;
    }

    size_t PendingChecks::RetainOnly(const std::set<int64_t>& slotLocations) {
        size_t dropped = 0;
        for (auto it = m_pending.begin(); it != m_pending.end();) {
            if (slotLocations.count(*it)) { ++it; } else { it = m_pending.erase(it); ++dropped; }
        }
        if (dropped) Save();
        return dropped;
    }

    ReconcileResult PendingChecks::Reconcile(const std::set<int64_t>& serverChecked,
                                             const std::set<int64_t>& serverMissing) {
        ReconcileResult r;
        for (int64_t id : serverChecked) Confirm(id);
        std::set<int64_t> slotLocs = serverMissing;
        slotLocs.insert(serverChecked.begin(), serverChecked.end());
        if (!slotLocs.empty()) r.dropped = RetainOnly(slotLocs);  // no location list = can't judge, keep everything
        r.toSend = Pending();
        return r;
    }

    bool PendingChecks::Save() const {
        if (m_path.empty()) return true;
        const std::string tmp = m_path + ".tmp";
        bool ok = false;
        {
            std::ofstream f(tmp, std::ios::trunc);
            if (f.is_open()) {
                for (int64_t id : m_pending) f << id << "\n";
                for (const auto& g : m_goals) f << "GOAL:" << g << "\n";
                f.flush();
                ok = static_cast<bool>(f);
            }
        }
        if (ok) {
            std::error_code ec;
            std::filesystem::rename(tmp, m_path, ec);  // replaces an existing file on Windows
            if (ec) {
                std::filesystem::remove(tmp, ec);
                ok = false;
            }
        }
        m_saveFailed = !ok;
        return ok;
    }

}  // namespace haloap
