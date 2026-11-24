#pragma once
// Pure logic of the CE effect queue (ADR-005). No game memory, no Windows, no clock of its own (the caller passes
// `nowMs`), so mcc-ap/tests/test_effects.cpp can drive it with a fake clock. NOT thread-safe: the owner (effects.cpp) locks.
//
// Rules (QUESTIONS #15 defaults, research 3.1/3.2): an effect is HELD, never dropped, until it can apply; at most one
// effect per kMinGapMs; nothing while not in a mission, dead or in a cutscene (phase A can only observe "in mission": the
// engine reads for dead/cinematic need the phase B frame hook, so those two gate fields stay at their permissive value);
// a settle delay after a mission load (the load hook fires before the scenario and the HS runtime are up); traps wait
// longer after a load; timed effects get their undo command at expiry; a mission change drops the timers and defers the undo
// commands to the next mission (undoing something that is not active is harmless: set false / physics_constants_reset /
// game_speed 1), so a stale timer can never fire into a different mission.
// Gravity and game speed live in checkpointed game state (QA M1): a checkpoint taken while one is active brings it back after a
// revert with no timer left. So once an effect of such a group (kStateGroups) has run, its neutral command is re-posted every
// kReassertMs whenever no effect of that group is active, and once in every following mission (idempotent).
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "effect_table.h"

namespace haloap {
namespace fx {

    constexpr uint64_t kMinGapMs = 10000;          // QUESTIONS #15: at most one effect per 10 s
    constexpr uint64_t kSettleMs = 8000;           // after a mission load: scenario + hs runtime + player unit must exist
    constexpr uint64_t kTrapSettleMs = 15000;      // traps: research 3.1.6 "never within 10 s of a mission load" (+ settle margin)
    constexpr size_t kQueueMax = 64;               // research 3.2 (a full queue is not an error: the launcher keeps the item in flight)
    constexpr uint64_t kReassertMs = 10000;        // QA M1: neutral gravity / speed re-asserted while nothing of that group runs
    constexpr uint64_t kRetryMs = 2000;            // after a failed post
    constexpr const char* kStateGroups[] = { "gravity", "speed" };

    struct Gate {
        bool inMission = false;
        bool engineReady = false;   // engine object + original slot-9 function resolved
        bool alive = true;          // phase A cannot read it (see header): callers leave it true
        bool cutscene = false;      // phase A cannot read it: callers leave it false
    };

    struct Action {
        enum Kind { Apply, Undo, Cleanup, Reassert } kind;
        int id = 0;                 // item id (Apply) or the id whose undo this is
        long index = -1;            // AP item index (Apply only)
        std::string command;        // HaloScript, without "HS: "
    };

    enum class Enqueue { Queued, Duplicate, AlreadyApplied, UnknownId, Full };

    // "EFFECT: <id> <index>" -> (id, index). Strict: two non-negative decimal integers, nothing else.
    inline std::optional<std::pair<int, long>> ParseEffectMessage(std::string_view m) {
        constexpr std::string_view p = "EFFECT: ";
        if (m.substr(0, p.size()) != p) return std::nullopt;
        m.remove_prefix(p.size());
        auto num = [&m](long long& out) {
            size_t n = 0;
            long long v = 0;
            while (n < m.size() && m[n] >= '0' && m[n] <= '9' && n < 10) v = v * 10 + (m[n++] - '0');
            if (n == 0 || (n < m.size() && m[n] != ' ' && m[n] != '\0')) return false;
            out = v;
            m.remove_prefix(n);
            return true;
        };
        long long id = 0, idx = 0;
        if (!num(id)) return std::nullopt;
        if (m.empty() || m.front() != ' ') return std::nullopt;
        m.remove_prefix(1);
        if (!num(idx) || !m.empty() || id > 0x7FFFFFFF || idx > 0x7FFFFFFF) return std::nullopt;
        return std::make_pair(static_cast<int>(id), static_cast<long>(idx));
    }

    class EffectQueue {
    public:
        // The launcher re-sends everything it has not seen applied after each (re)connect, so an index already queued or
        // applied in this process is a Duplicate (ignored; the first copy is still in flight or done).
        Enqueue Add(int id, long index) {
            if (!FindEffect(id)) return Enqueue::UnknownId;
            if (m_applied.count(index)) return Enqueue::AlreadyApplied;  // its ack may have been lost: the caller re-acks
            if (m_seen.count(index)) return Enqueue::Duplicate;
            if (m_pending.size() >= kQueueMax) return Enqueue::Full;
            m_seen.insert(index);
            m_pending.push_back({ id, index });
            return Enqueue::Queued;
        }

        // A mission was (re)loaded: timers are dropped, their undo commands run once in the next playable window, and
        // the settle clock restarts. Pending effects stay queued.
        void OnMissionLoad(uint64_t nowMs) {
            VoidTimers();
            m_enteredMs = nowMs;
        }

        // The post of `a` failed (engine/module not ready, free pool empty ...): nothing happened, so undo the bookkeeping and
        // try again after kRetryMs. An apply goes back to the FRONT of the queue; a failed undo/cleanup is kept (QA m1/m2).
        void Failed(const Action& a, uint64_t nowMs) {
            m_retryAtMs = nowMs + kRetryMs;
            if (a.kind == Action::Apply) {
                const EffectDef* d = FindEffect(a.id);
                if (d && d->durationMs > 0 && d->undo[0]) {
                    auto it = m_active.find(d->group);
                    if (it != m_active.end() && it->second.id == a.id) m_active.erase(it);
                }
                m_applied.erase(a.index);
                m_pending.push_front({ a.id, a.index });
                m_hasApplied = m_hadApplied;
                m_lastApplyMs = m_prevApplyMs;
            } else if (a.kind == Action::Undo || a.kind == Action::Cleanup) {
                m_cleanup.insert(a.command);
            }
        }

        // Call every 100-250 ms from the worker thread. Returns the commands to post now, in order.
        std::vector<Action> Tick(uint64_t nowMs, const Gate& g) {
            std::vector<Action> out;
            if (g.inMission && !m_wasIn) m_enteredMs = nowMs;  // entered a mission: settle clock
            if (!g.inMission && m_wasIn) VoidTimers();  // left it: this mission's timers are void
            m_wasIn = g.inMission;
            if (!g.inMission || !g.engineReady) return out;  // hold everything (timers cannot run: they are void outside)

            for (auto it = m_active.begin(); it != m_active.end();) {  // expiry first: an undo is never delayed by the gap
                if (nowMs >= it->second.expireMs) {
                    out.push_back({ Action::Undo, it->second.id, -1, it->second.undo });
                    auto st = m_state.find(it->first);
                    if (st != m_state.end()) st->second.nextMs = nowMs + kReassertMs;  // the undo itself is the first neutral post
                    it = m_active.erase(it);
                } else ++it;
            }
            if (Since(nowMs, m_enteredMs) < kSettleMs) return out;
            if (nowMs >= m_retryAtMs) {
                for (const auto& u : m_cleanup) out.push_back({ Action::Cleanup, 0, -1, u });
                m_cleanup.clear();
            }
            for (auto& [group, st] : m_state) {  // QA M1: keep neutral while no effect of the group runs
                if (!st.touched || m_active.count(group) || nowMs < st.nextMs || nowMs < m_retryAtMs) continue;
                out.push_back({ Action::Reassert, 0, -1, GroupUndo(group) });
                st.nextMs = nowMs + kReassertMs;
            }

            if (nowMs < m_retryAtMs) return out;
            if (!g.alive || g.cutscene || (m_hasApplied && Since(nowMs, m_lastApplyMs) < kMinGapMs)) return out;
            for (auto it = m_pending.begin(); it != m_pending.end(); ++it) {  // first item that may go now; order otherwise kept
                const EffectDef* d = FindEffect(it->id);
                if (d->trap && Since(nowMs, m_enteredMs) < kTrapSettleMs) continue;
                out.push_back({ Action::Apply, it->id, it->index, d->apply });
                if (d->durationMs > 0 && d->undo[0]) {
                    // same group: the new effect replaces the old timer (its undo would end the new one early)
                    m_active[d->group] = { it->id, nowMs + static_cast<uint64_t>(d->durationMs), d->undo };
                }
                if (IsStateGroup(d->group)) {
                    auto& st = m_state[d->group];
                    st.touched = true;
                    st.everTouched = true;
                    st.nextMs = nowMs + kReassertMs;
                }
                m_applied.insert(it->index);
                m_hadApplied = m_hasApplied;
                m_prevApplyMs = m_lastApplyMs;
                m_lastApplyMs = nowMs;
                m_hasApplied = true;
                m_pending.erase(it);
                break;
            }
            return out;
        }

        size_t Pending() const { return m_pending.size(); }
        size_t ActiveTimers() const { return m_active.size(); }
        size_t DeferredUndos() const { return m_cleanup.size(); }
        bool Applied(long index) const { return m_applied.count(index) != 0; }

    private:
        // The clock is read on two threads (worker tick, mission-load hook): a start a few ms "in the future" counts as 0
        static uint64_t Since(uint64_t now, uint64_t then) { return now >= then ? now - then : 0; }
        static bool IsStateGroup(const std::string& g) {
            for (const char* s : kStateGroups) if (g == s) return true;
            return false;
        }
        static std::string GroupUndo(const std::string& group) {
            for (size_t i = 0; i < kEffectCount; i++)
                if (group == kEffects[i].group && kEffects[i].undo[0]) return kEffects[i].undo;
            return {};
        }
        // Mission change: running timers end (their undo is deferred to the next playable window), the per-mission re-assert
        // stops, and every state group that was ever used gets its neutral command once in the next mission.
        void VoidTimers() {
            for (auto& [group, a] : m_active) m_cleanup.insert(a.undo);
            m_active.clear();
            for (auto& [group, st] : m_state) {
                if (st.everTouched) m_cleanup.insert(GroupUndo(group));
                st.touched = false;
            }
        }
        struct Item { int id; long index; };
        struct State { bool touched = false, everTouched = false; uint64_t nextMs = 0; };
        std::map<std::string, State> m_state;
        std::set<long> m_applied;
        uint64_t m_retryAtMs = 0;
        uint64_t m_prevApplyMs = 0;
        bool m_hadApplied = false;
        struct Active { int id; uint64_t expireMs; std::string undo; };
        std::deque<Item> m_pending;
        std::set<long> m_seen;
        std::map<std::string, Active> m_active;
        std::set<std::string> m_cleanup;
        uint64_t m_enteredMs = 0;
        uint64_t m_lastApplyMs = 0;
        bool m_hasApplied = false;
        bool m_wasIn = false;
    };

}  // namespace fx
}  // namespace haloap
