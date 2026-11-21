#pragma once
// Location IDs not yet confirmed by the AP server, plus the "goal reached" flag, persisted per seed+slot
// (one int64 per line, and a line "GOAL" when the goal was reached). Pure C++20, no AP dependencies.
// NOT thread-safe: the owner locks.

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace haloap {

    // "haloap_pending_<16 hex>.txt", hash of seed + slot name (always a safe filename). Keyed by seed so a new
    // seed on the same server address and slot name never inherits another seed's checks (Phase 2 QA B1).
    std::string PendingFileName(const std::string& seed, const std::string& slot);

    struct ReconcileResult {
        std::vector<int64_t> toSend;  // pending IDs that are slot locations the server hasn't checked
        bool sendGoal = false;        // goal was reached in this seed; GOAL is idempotent, so resend on every connect
        size_t dropped = 0;           // pending IDs that aren't locations of this slot
    };

    class PendingChecks {
    public:
        // Empty path = memory only. Otherwise every change is saved atomically (temp file + rename).
        explicit PendingChecks(std::string path = {});

        // Switches to the seed-keyed file: loads it and merges anything queued in memory before the seed was known.
        void Bind(const std::string& path);

        // Reads the file; lines that are not a positive int64 or "GOAL" are ignored. Missing file = empty.
        void Load();

        // Queues an ID. Returns true if newly queued; false if invalid (<= 0), already pending,
        // or already confirmed by the server.
        bool Add(int64_t id);

        // Server reports the ID checked: drops it from pending and remembers it. Returns true if it was pending.
        bool Confirm(int64_t id);

        // Drops pending IDs that are not locations of the connected slot (e.g. the goal mission's completion,
        // or a kill check when killsanity is off), so they aren't resent forever. Returns how many were dropped.
        size_t RetainOnly(const std::set<int64_t>& slotLocations);

        // On slot connect: confirm what the server has, drop non-locations (only if the server sent any locations),
        // and report what to send.
        ReconcileResult Reconcile(const std::set<int64_t>& serverChecked, const std::set<int64_t>& serverMissing);

        void SetGoalReached() { if (!m_goal) { m_goal = true; Save(); } }
        bool GoalReached() const { return m_goal; }

        bool IsPending(int64_t id) const { return m_pending.count(id) != 0; }
        bool IsConfirmed(int64_t id) const { return m_confirmed.count(id) != 0; }
        std::vector<int64_t> Pending() const { return { m_pending.begin(), m_pending.end() }; }
        size_t Size() const { return m_pending.size(); }
        const std::string& Path() const { return m_path; }

        // Returns false if the file could not be written (logged once by the caller).
        bool Save() const;
        bool LastSaveFailed() const { return m_saveFailed; }

    private:
        std::string m_path;
        std::set<int64_t> m_pending;
        std::set<int64_t> m_confirmed;
        bool m_goal = false;
        mutable bool m_saveFailed = false;
    };

}  // namespace haloap
