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

    // slot_data "goal_mode" (PLAN 6.G, QUESTIONS #13). A seed without the key is PerGame. Any other value than 0 / 1 is Unknown
    // (a newer apworld): the goal is then unreachable and nothing is auto-sent, rather than guessed.
    enum class GoalMode { PerGame, OneFinal, Unknown };

    // What FlushPending does about the goal, for the flags set so far and the server's missing locations (pure, see Decide).
    struct GoalDecision {
        bool sendGoal = false;
        std::vector<int64_t> release;  // missing locations of finished games, to send now (per_game with several games only)
    };

    class GameProgress {
    public:
        // Mission lookups (static tables). Unknown code / non-mission location = nullopt.
        static std::optional<MissionRef> ByCode(const std::string& code);
        static std::optional<MissionRef> ByLocation(int64_t id);  // a mission-completion location (CE or H2)
        static bool IsH2Code(const std::string& code);
        // H2 chapter location for (mission code, title index); 03b's title indices 0,1,2 all give chapter 1.
        static std::optional<ChapterRef> H2Chapter(const std::string& code, int titleIndex);

        // Which game a location ID belongs to (ADR-001 / ADR-004 ID scheme), or nullopt for an ID of no known game.
        // Game digit g = 1 CE, 2 H2 (3 H3, 4 H4, 5 ODST, 6 Reach are reserved and give nullopt until they exist):
        //   mission / chapter / skull locations   g*100000 .. g*100000 + 99999      (CE 100000..199999, H2 200000..299999)
        //   kill milestones, species, infection   1'000'000'000 + g*10'000'000 .. + 9'999'999   (CE 1'010'000'000..1'019'999'999)
        // Item IDs (skull disablers 90000+, effects 80000+) are below 100000 and never locations.
        static std::optional<Game> GameOfLocation(int64_t id);

        // Slot settings from slot_data (every key read with .find). Missing `ce_enabled` = old seed = CE on with its
        // final from `final_mission` (default The Maw, as before). Otherwise a final that is "", missing or unknown, or
        // belongs to a disabled game, is "none". Returns the games whose final mission was completed before this call
        // (completions seen while the config was unknown); that history is then dropped.
        std::vector<Game> Configure(const nlohmann::json& slotData);
        bool Configured() const { return m_configured; }
        // False only when the config is known and the game is off: its events are then ignored.
        bool Wants(Game g) const { return !m_configured || Enabled(g); }
        bool Enabled(Game g) const { return g == Game::CE ? m_ce : m_h2; }
        GoalMode Mode() const { return m_mode; }

        // A mission was completed. Returns the game when it was that game's final mission (config known, game on).
        std::optional<Game> OnMissionComplete(const MissionRef& m);

        // Goal: configured, at least one game on, and
        //  - per_game: every enabled game has a final mission whose flag is in `flags` (GameTag values). A game with
        //    no final makes the goal unreachable.
        //  - one_final: the goal game's flag is in `flags`. Another game's final mission is an ordinary mission: it is
        //    no final here (FinalIndex -1), so it never sets a flag and never counts. The goal game is slot_data
        //    "goal_game"; with one game enabled that game; with several and a missing / unknown / disabled goal_game: none.
        bool GoalReached(const std::set<std::string>& flags) const;

        // What to do now: sendGoal = GoalReached. release = (per_game, two or more games enabled, goal not yet reached)
        // every server-missing location whose game's final mission is flagged. A single enabled game never releases,
        // so a one-game seed behaves exactly as before 6.G. Idempotent: the same flags + missing give the same answer,
        // and a location the server already has is not in `missing`.
        // releaseAllowed: the host's room permits a manual release now (ReleaseAllowed below). When false, release is
        // always empty: the user chose to respect the host's release setting (QUESTIONS #13, 2026-11-24); the remaining
        // checks then stay open to collect, and GOAL is unaffected.
        GoalDecision Decide(const std::set<std::string>& flags, const std::set<int64_t>& missing, bool releaseAllowed) const;

        // The room's "release" permission (RoomInfo/RoomUpdate permissions; bit 1 = ENABLED, manual use any time).
        // A game finishing is not the slot's goal, so only ENABLED / AUTO_ENABLED allow it; GOAL-only, AUTO, DISABLED,
        // or an unknown permission (nullopt) do not.
        static bool ReleaseAllowed(std::optional<int> permission) { return permission && (*permission & 1) != 0; }

        // Launcher -> DLL state: COMPLETED:, FINAL_MISSION: (-1 = none), and, only when H2 is on, H2_COMPLETED:
        // (mission numbers) and H2_FINAL: (0 = none). CE lines are byte-identical to the old launcher for CE seeds.
        std::vector<std::string> DllMessages(const std::set<int64_t>& checked) const;

    private:
        int FinalIndex(Game g) const;  // CE index / H2 n, or -1 = none (also -1 for a game whose final is not the goal, one_final)
        std::optional<Game> GoalGame() const;  // one_final only
        bool m_configured{ false };
        GoalMode m_mode{ GoalMode::PerGame };
        std::string m_goalGame;
        bool m_ce{ true };
        bool m_h2{ false };
        std::string m_ceFinal{ "The Maw" };
        std::string m_h2Final;
        std::set<int64_t> m_early;  // completions before Configure
    };

}  // namespace haloap
