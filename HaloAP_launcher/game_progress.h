#pragma once
// Per-game mission / goal logic (ADR-004 decisions 3 and 7). Pure: no AP client, no I/O. Not thread-safe; the owner
// (APBridge, m_progressMutex) locks. Mission codes pick the game: CE codes (a10) never collide with H2 codes
// (03a_oldmombasa).
#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace haloap {

    enum class Game { CE, H2 };
    inline const char* GameTag(Game g) { return g == Game::CE ? "CE" : "H2"; }  // also the pending-file GOAL:<tag>

    // index: CE 0..9 (campaign order); H2 n = 2..15 (MCC order; n = 1 The Heretic is a cutscene, never a check).
    struct MissionRef { Game game; int index; int64_t id; const char* name; };
    struct ChapterRef { int64_t id; const char* name; };

    class GameProgress {
    public:
        // Mission lookups (static tables). Unknown code / non-mission location = nullopt.
        static std::optional<MissionRef> ByCode(const std::string& code);
        static std::optional<MissionRef> ByLocation(int64_t id);  // a mission-completion location (CE or H2)
        static bool IsH2Code(const std::string& code);
        // H2 chapter location for (mission code, title index); 03b's title indices 0,1,2 all give chapter 1.
        static std::optional<ChapterRef> H2Chapter(const std::string& code, int titleIndex);

        // Slot settings from slot_data (every key read with .find). Missing `ce_enabled` = old seed = CE on with its
        // final from `final_mission` (default The Maw, as before). Otherwise a final that is "", missing or unknown, or
        // belongs to a disabled game, is "none". Returns the games whose final mission was completed before this call
        // (completions seen while the config was unknown); that history is then dropped.
        std::vector<Game> Configure(const nlohmann::json& slotData);
        bool Configured() const { return m_configured; }
        // False only when the config is known and the game is off: its events are then ignored.
        bool Wants(Game g) const { return !m_configured || Enabled(g); }
        bool Enabled(Game g) const { return g == Game::CE ? m_ce : m_h2; }

        // A mission was completed. Returns the game when it was that game's final mission (config known, game on).
        std::optional<Game> OnMissionComplete(const MissionRef& m);

        // Goal: configured, at least one game on, and every enabled game has a final mission whose flag is in
        // `flags` (GameTag values). A game with no final makes the goal unreachable.
        bool GoalReached(const std::set<std::string>& flags) const;

        // Launcher -> DLL state: COMPLETED:, FINAL_MISSION: (-1 = none), and, only when H2 is on, H2_COMPLETED:
        // (mission numbers) and H2_FINAL: (0 = none). CE lines are byte-identical to the old launcher for CE seeds.
        std::vector<std::string> DllMessages(const std::set<int64_t>& checked) const;

    private:
        int FinalIndex(Game g) const;  // CE index / H2 n, or -1 = none
        bool m_configured{ false };
        bool m_ce{ true };
        bool m_h2{ false };
        std::string m_ceFinal{ "The Maw" };
        std::string m_h2Final;
        std::set<int64_t> m_early;  // completions before Configure
    };

}  // namespace haloap
