#pragma once

#include <apclient.hpp>
#include <memory>
#include <string>
#include <atomic>
#include <mutex>
#include <set>
#include <cstdint>
#include <list>
#include <functional>
#include <vector>
#include "pending_checks.h"
#include "telemetry_log.h"
#include "kill_tracker.h"
#include "game_progress.h"

namespace haloap {

    class APBridge {
    public:
        APBridge();
        ~APBridge();

        // Start connecting to an AP server. Non-blocking; poll() to make progress.
        // Returns false only if the client couldn't be constructed (bad params, etc.).
        bool Start(const std::string& serverUri,
            const std::string& game,
            const std::string& slot,
            const std::string& password);

        // Must be called regularly (e.g. every 50ms) to service the socket.
        void Poll();

        // Shut down. Safe to call multiple times.
        void Stop();

        // Handle a message from the DLL (forwarded from the pipe).
        // Returns true if the message was recognized and handled.
        bool HandleDllMessage(const std::string& message);

        // Convenience: send a location check. Deduplicates within this session.
        // notifyDll = false skips the completion-state push to the DLL (kill checks don't change it).
        void SendLocation(int64_t locationId, bool notifyDll = true);

        bool IsConnected() const { return m_slotConnected.load(); }
        bool IsSocketConnected() const { return m_socketConnected.load(); }
        
        void SetSendToDll(std::function<void(const std::string&)> fn) {
            m_sendToDll = std::move(fn);
        }
        
        void ReplayBufferedItems();

        // Directory for the pending-checks file (same as haloap_config.txt, trailing separator).
        // Call before Start(); empty = keep pending checks in memory only.
        // Also the directory for haloap_telemetry_*.jsonl (always on in this build).
        // `buildTag` = first word of the MCC install's build_tag.txt (empty if unknown); goes in the telemetry `session` line.
        void SetStateDir(const std::string& dir, const std::string& buildTag = "") {
            m_stateDir = dir;
            m_telemetry = std::make_unique<TelemetryLog>(dir, buildTag);
        }

        // DLL pipe state for telemetry `pipe` lines. "connected" is emitted automatically on the DLL's HELLO message;
        // call NotePipeState(false) when the pipe drops (also done by Stop()). Repeated identical states are ignored,
        // and a HELLO while already connected logs disconnected then connected.
        void NotePipeState(bool connected) {
            bool was = m_pipeUp.exchange(connected);  // atomic: called from the pipe and monitor threads (QA n1)
            if (!m_telemetry) return;
            if (!connected && !was) return;           // already down
            if (connected && was) m_telemetry->Pipe(false);  // a second HELLO: the DLL reconnected
            m_telemetry->Pipe(connected);
        }

    private:
        // APClient callbacks. These run on the poll thread.
        void OnSocketConnected();
        void OnSocketError(const std::string& error);
        void OnSocketDisconnected();
        void OnRoomInfo();
        void OnSlotConnected(const nlohmann::json& slotData);
        void OnSlotRefused(const std::list<std::string>& reasons);
        void OnItemsReceived(const std::list<APClient::NetworkItem>& items);
        void OnPrintJson(const APClient::PrintJSONArgs& args);
        void SendCompletionState();
        
        std::function<void(const std::string&)> m_sendToDll;

        std::unique_ptr<APClient> m_client;
        std::string m_game;
        std::string m_slot;
        std::string m_password;
        
        std::mutex m_itemBufferMutex;
        std::vector<int64_t> m_itemBuffer;
        int m_skullsanityTier{ -1 };

        std::atomic<bool> m_socketConnected{ false };
        std::atomic<bool> m_slotConnected{ false };
        std::atomic<bool> m_stopped{ false };

        // Locations not yet confirmed by the server (persisted). Guarded by m_pendingMutex.
        // Pipe thread only adds; the poll thread sends (m_flushRequested) and confirms.
        std::mutex m_pendingMutex;
        PendingChecks m_pending;
        std::string m_stateDir;
        std::atomic<bool> m_pipeUp{ false };
        std::unique_ptr<TelemetryLog> m_telemetry;  // set by SetStateDir; internally locked
        std::set<int64_t> m_inFlight;  // sent on this connection, awaiting confirmation (poll thread only)
        std::atomic<bool> m_flushRequested{ false };
        // "Re-evaluate the goal" request (set by either thread; cleared by the poll thread in FlushPending).
        std::atomic<bool> m_goalRequested{ false };
        // Goal state, two parts, never held together (no nesting): the per-game flags live in m_pending (guarded by
        // m_pendingMutex; the pipe thread sets a flag on a final mission's completion, the poll thread sets the ones
        // seen before the slot connected); the slot config + pre-connect completions live in m_progress (guarded by
        // m_progressMutex; pipe thread: OnMissionComplete, DllMessages; poll thread: Configure). GOAL is sent by
        // the poll thread only, from copies of both.
        // Held across copy + build + send of the DLL completion state, so an older state is never sent last (QA 8.H2.7b m5).
        // Taken first; never acquired while holding any other mutex of this class.
        std::mutex m_sendStateMutex;
        std::mutex m_progressMutex;
        GameProgress m_progress;
        std::atomic<bool> m_saveFailureLogged{ false };
        void FlushPending();  // poll thread only
        void OnLocationChecked(const std::list<int64_t>& locations);

        std::set<int64_t> m_checkedLocations;
        std::mutex m_checkedMutex;

        // Kill checks (ADR-001 Amendment A). Thread rules:
        //  - pipe thread: HandleDllMessage -> FeedKillTracker (KillTracker::OnEvent) and OnMissionLoad. It never
        //    touches m_client; SendLocation only queues + persists under m_pendingMutex.
        //  - poll thread: OnSlotConnected reads m_client (checked/missing locations, slot_data), then under
        //    m_killMutex stores the slot locations, calls KillTracker::SetConfig and sends what that returns.
        //  - Every use of m_killTracker, m_slotLocations happens under m_killMutex (lock order: m_killMutex, then
        //    m_pendingMutex inside SendLocation; never the reverse).
        // The tracker always tracks counts/species, so kills before the slot connects are not lost; it emits
        // nothing until SetConfig. Emitted IDs are sent only if they are slot locations (the server's checked +
        // missing sets): a milestone above this seed's kill_milestone_max_percent cap is not one.
        std::mutex m_killMutex;
        std::unique_ptr<KillTracker> m_killTracker;
        std::set<int64_t> m_slotLocations;  // server checked + missing; empty until the slot connects
        std::string m_lastSeed;             // seed of the last slot connect (kill history resets on a change)
        void FeedKillTracker(const AiEvent& ev);
        void SendKillChecks(const std::vector<int64_t>& earned);  // caller holds m_killMutex
    };

}  // namespace haloap