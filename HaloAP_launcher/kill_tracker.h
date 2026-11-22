#pragma once
// Pure, dependency-free kill-check tracker (PLAN 5.3/5.4, ADR-001 Amendment A).
// Turns the DLL's KILL and ERASE events into newly earned milestone / species location IDs. An ERASE (a script
// despawning an actor) counts like a kill (QUESTIONS #9); a scripted unit_kill death does not. A kill is counted by
// the actor's VARIANT (hostile + countable per map data), not by its squad (ADR-003 amendment 2).
#include "pipe_protocol.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace haloap {

    // Per-mission tables; produced by tools/h1/gen_launcher_kills.py (Data/kill_map.h) or injected by tests.
    struct SquadKey { int enc; int sq; };
    struct SpeciesLoc { const char* species; int64_t id; };
    struct VariantSpecies { int variant; const char* species; };  // variant = tag index (var & 0xFFFF)
    struct MissionKillTable {
        int missionNo;                       // 1..10, campaign order (a10 = 1 ... d40 = 10)
        std::vector<SquadKey> squads;        // hostile catalog squads (kept for future squad checks; NOT used to count)
        std::vector<SpeciesLoc> species;
        std::vector<VariantSpecies> variants;
        // ADR-003 amendment 2: actor-variant tag indices (var & 0xFFFF) of hostile, countable actors. A death/erase
        // counts iff its variant is in this list; the squad/encounter at death time is irrelevant (script migration
        // puts actors into empty destination squads, some have enc/sq = -1).
        std::vector<int> hostileVariants;
    };
    using KillTables = std::map<std::string, MissionKillTable>;

    // Milestone location ID for "kill count `count` in mission `missionNo`" (ADR-001 Amendment A). A formula, not a
    // table: the apworld's data/kill_ids.py milestone_id() is the same thing, and tests/test_kill_ids_agree.py
    // fails if the constants drift.
    constexpr int64_t kKillBase = 1000000000LL;
    constexpr int64_t kGameCe = 1;
    constexpr int kMaxMilestoneCount = 9999;
    constexpr int64_t MilestoneId(int missionNo, int count) {
        return kKillBase + kGameCe * 10000000LL + static_cast<int64_t>(100 + missionNo) * 10000LL + count;
    }

    // Not thread-safe; the caller serializes access.
    //
    // The tracker ALWAYS tracks (per-mission peak kill counts and the species locations seen), whether or not the
    // slot options are known yet; it only EMITS IDs once SetConfig() has been called. SetConfig() returns everything
    // earned so far under the new settings, so kills made before the slot connected are not lost, and a reconnect
    // to a different seed re-evaluates from the history. The caller must filter emitted IDs to the slot's real
    // locations (this class has no cap: an ID beyond the seed's kill_milestone_max_percent is not a location).
    //
    // Counts are PER MISSION RUN: OnMissionLoad() resets the unique-kill set and count, so a milestone has to be
    // reached within one attempt of the mission. A checkpoint revert does not reset the count; the actor-handle
    // dedupe means re-killing respawned actors with the same handle is not double counted.
    // Within one config, each location ID is returned at most once (PendingChecks de-dups across restarts).
    class KillTracker {
    public:
        explicit KillTracker(KillTables tables);

        // Slot settings: milestone every `interval` counted kills (0 = none; clamped to 0..100) and whether the
        // species checks are on. Defaults (0, false) emit nothing. Returns every ID earned so far under the new
        // settings (the emitted-set restarts, so a changed seed gets a full re-evaluation).
        std::vector<int64_t> SetConfig(int interval, bool species);

        // Forgets the per-mission peaks and species seen in earlier runs (a different seed connected). The current
        // run's count is kept: those kills happen in the new seed's session from here on.
        void ResetHistory() { m_peak.clear(); m_speciesSeen.clear(); m_emitted.clear(); }

        // Starts a new run of `code`. Empty or unknown code: nothing is tracked until the next valid load.
        void OnMissionLoad(const std::string& code);

        // Newly earned location IDs for this event (usually empty).
        std::vector<int64_t> OnEvent(const AiEvent& ev);

        int Count() const { return static_cast<int>(m_seen.size()); }

    private:
        KillTables m_tables;
        const MissionKillTable* m_cur{ nullptr };
        std::set<uint32_t> m_seen;                 // actor handles counted this run
        std::map<int, int> m_peak;                 // mission_no -> highest count reached in any run
        std::set<int64_t> m_speciesSeen;           // species location IDs whose first kill happened (any run)
        std::set<int64_t> m_emitted;               // IDs returned since the last SetConfig
        int m_interval{ 0 };
        bool m_species{ false };
    };

}  // namespace haloap
