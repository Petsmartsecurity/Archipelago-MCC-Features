#pragma once
// Pure, dependency-free kill-check tracker (PLAN 5.3, ADR-001 "milestones" mode).
// Turns the DLL's KILL events into newly earned milestone / species location IDs.
#include "pipe_protocol.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace haloap {

    // Per-mission tables; produced by tools/h1/gen_launcher_kills.py (Data/kill_map.h) or injected by tests.
    struct SquadKey { int enc; int sq; };
    struct Milestone { int threshold; int64_t id; };
    struct SpeciesLoc { const char* species; int64_t id; };
    struct VariantSpecies { int variant; const char* species; };  // variant = tag index (var & 0xFFFF)
    struct MissionKillTable {
        std::vector<SquadKey> squads;        // hostile catalog squads
        std::vector<Milestone> milestones;
        std::vector<SpeciesLoc> species;
        std::vector<VariantSpecies> variants;
    };
    using KillTables = std::map<std::string, MissionKillTable>;

    // Not thread-safe; the caller serializes access.
    //
    // Counts are PER MISSION RUN: OnMissionLoad() resets the unique-kill set and count, so a milestone has to be
    // reached within one attempt of the mission (thresholds are <= half the mission's Normal enemies, so this is
    // always achievable). A checkpoint revert does not reset the count; the actor-handle dedupe means re-killing
    // respawned actors with the same handle is not double counted.
    // Each location ID is returned at most once per tracker lifetime (PendingChecks de-dups across restarts).
    class KillTracker {
    public:
        explicit KillTracker(KillTables tables);

        // Starts a new run of `code`. Empty or unknown code: nothing is tracked until the next valid load.
        void OnMissionLoad(const std::string& code);

        // Newly earned location IDs for this event (usually empty).
        std::vector<int64_t> OnEvent(const AiEvent& ev);

        int Count() const { return static_cast<int>(m_seen.size()); }

    private:
        KillTables m_tables;
        const MissionKillTable* m_cur{ nullptr };
        std::set<uint32_t> m_seen;     // actor handles counted this run
        std::set<int64_t> m_emitted;   // IDs returned so far (lifetime)
    };

}  // namespace haloap
