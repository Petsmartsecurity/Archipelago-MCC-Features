#include "kill_tracker.h"

#include <cstring>
#include <utility>

namespace haloap {

    KillTracker::KillTracker(KillTables tables) : m_tables(std::move(tables)) {}

    void KillTracker::OnMissionLoad(const std::string& code) {
        m_seen.clear();
        auto it = m_tables.find(code);
        m_cur = (it == m_tables.end()) ? nullptr : &it->second;
    }

    std::vector<int64_t> KillTracker::OnEvent(const AiEvent& ev) {
        std::vector<int64_t> earned;
        if (!m_cur) return earned;
        // ERASE (a script despawning a hostile actor) counts as a kill, by the user's choice (QUESTIONS #9, ADR-003).
        // A scripted unit_kill (death with unit kill flag 0x20) still does not count.
        if (ev.kill && ev.ekf != 0xFF && (ev.ekf & 0x20)) return earned;
        bool hostile = false;
        for (const auto& s : m_cur->squads) {
            if (s.enc == ev.enc && s.sq == ev.sq) { hostile = true; break; }
        }
        if (!hostile) return earned;
        if (!m_seen.insert(ev.ah).second) return earned;  // same actor already counted this run

        auto emit = [&](int64_t id) {
            if (m_emitted.insert(id).second) earned.push_back(id);
        };
        int count = Count();
        for (const auto& m : m_cur->milestones) {
            if (count >= m.threshold) emit(m.id);
        }
        int variant = static_cast<int>(ev.var & 0xFFFF);
        for (const auto& v : m_cur->variants) {
            if (v.variant != variant) continue;
            for (const auto& s : m_cur->species) {
                if (std::strcmp(s.species, v.species) == 0) emit(s.id);
            }
            break;
        }
        return earned;
    }

}  // namespace haloap
