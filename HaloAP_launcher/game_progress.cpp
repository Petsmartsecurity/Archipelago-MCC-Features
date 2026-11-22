#include "game_progress.h"
#include "data/h2_map.h"

#include <algorithm>

namespace haloap {

    namespace {
        struct CeRow { const char* code; const char* name; int64_t id; };  // name = apworld final_mission value
        constexpr CeRow kCe[10] = {
            {"a10", "The Pillar of Autumn", 101000}, {"a30", "Halo (CE)", 102000},
            {"a50", "The Truth and Reconciliation", 103000}, {"b30", "The Silent Cartographer", 104000},
            {"b40", "Assault on the Control Room", 105000}, {"c10", "343 Guilty Spark", 106000},
            {"c20", "The Library", 107000}, {"c40", "Two Betrayals", 108000},
            {"d20", "Keyes", 109000}, {"d40", "The Maw", 110000},
        };

        // Reads slot_data[key] as bool/int; missing or other types = fallback. Never operator[].
        bool Flag(const nlohmann::json& d, const char* key, bool fallback) {
            if (!d.is_object()) return fallback;
            auto it = d.find(key);
            if (it == d.end()) return fallback;
            if (it->is_boolean()) return it->get<bool>();
            if (it->is_number_integer()) return it->get<int64_t>() != 0;
            return fallback;
        }
        std::optional<std::string> Str(const nlohmann::json& d, const char* key) {
            if (!d.is_object()) return std::nullopt;
            auto it = d.find(key);
            if (it == d.end() || !it->is_string()) return std::nullopt;
            return it->get<std::string>();
        }
        std::string Join(const std::set<int>& v) {
            std::string s;
            for (int i : v) { if (!s.empty()) s += ","; s += std::to_string(i); }
            return s;
        }
    }

    std::optional<MissionRef> GameProgress::ByCode(const std::string& code) {
        for (int i = 0; i < 10; ++i)
            if (code == kCe[i].code) return MissionRef{ Game::CE, i, kCe[i].id, kCe[i].name };
        for (const auto& m : kH2Missions)
            if (code == m.code) return MissionRef{ Game::H2, m.n, m.id, m.name };
        return std::nullopt;
    }

    std::optional<MissionRef> GameProgress::ByLocation(int64_t id) {
        for (int i = 0; i < 10; ++i)
            if (id == kCe[i].id) return MissionRef{ Game::CE, i, id, kCe[i].name };
        for (const auto& m : kH2Missions)
            if (id == m.id) return MissionRef{ Game::H2, m.n, id, m.name };
        return std::nullopt;
    }

    bool GameProgress::IsH2Code(const std::string& code) {
        auto m = ByCode(code);
        return (m && m->game == Game::H2) || code == "00a_introduction";  // the cutscene has no row but is still H2
    }

    std::optional<ChapterRef> GameProgress::H2Chapter(const std::string& code, int titleIndex) {
        for (const auto& c : kH2Chapters)
            if (c.titleIndex == titleIndex && code == c.code) return ChapterRef{ c.id, c.name };
        return std::nullopt;
    }

    int GameProgress::FinalIndex(Game g) const {
        if (!Enabled(g)) return -1;
        if (g == Game::CE) {
            for (int i = 0; i < 10; ++i) if (m_ceFinal == kCe[i].name) return i;
        } else {
            for (const auto& m : kH2Missions) if (m_h2Final == m.name) return m.n;
        }
        return -1;
    }

    std::vector<Game> GameProgress::Configure(const nlohmann::json& d) {
        const bool oldSeed = !(d.is_object() && d.find("ce_enabled") != d.end());
        m_ce = Flag(d, "ce_enabled", true);
        m_h2 = Flag(d, "h2_enabled", false);
        m_ceFinal = Str(d, "final_mission").value_or(oldSeed ? "The Maw" : "");
        m_h2Final = Str(d, "h2_final_mission").value_or("");
        m_configured = true;
        std::vector<Game> done;
        for (Game g : { Game::CE, Game::H2 }) {
            int f = FinalIndex(g);
            if (f < 0) continue;
            int64_t id = kCe[0].id;
            if (g == Game::CE) id = kCe[f].id;
            else for (const auto& m : kH2Missions) if (m.n == f) id = m.id;
            if (m_early.count(id)) done.push_back(g);
        }
        m_early.clear();
        return done;
    }

    std::optional<Game> GameProgress::OnMissionComplete(const MissionRef& m) {
        if (!m_configured) { m_early.insert(m.id); return std::nullopt; }
        if (FinalIndex(m.game) == m.index) return m.game;
        return std::nullopt;
    }

    bool GameProgress::GoalReached(const std::set<std::string>& flags) const {
        if (!m_configured || (!m_ce && !m_h2)) return false;
        for (Game g : { Game::CE, Game::H2 }) {
            if (!Enabled(g)) continue;
            if (FinalIndex(g) < 0 || !flags.count(GameTag(g))) return false;
        }
        return true;
    }

    std::vector<std::string> GameProgress::DllMessages(const std::set<int64_t>& checked) const {
        std::set<int> ce, h2;
        for (int i = 0; i < 10; ++i) if (checked.count(kCe[i].id)) ce.insert(i);
        for (const auto& m : kH2Missions) if (checked.count(m.id)) h2.insert(m.n);
        std::vector<std::string> out = { "COMPLETED:" + Join(ce), "FINAL_MISSION:" + std::to_string(FinalIndex(Game::CE)) };
        if (m_h2) {
            out.push_back("H2_COMPLETED:" + Join(h2));
            out.push_back("H2_FINAL:" + std::to_string(std::max(0, FinalIndex(Game::H2))));
        }
        return out;
    }

}  // namespace haloap
