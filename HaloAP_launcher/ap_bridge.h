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
        void SendLocation(int64_t locationId);

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
        std::atomic<bool> m_goalRequested{ false };
        std::atomic<bool> m_saveFailureLogged{ false };
        void FlushPending();  // poll thread only
        void OnLocationChecked(const std::list<int64_t>& locations);

        std::set<int64_t> m_checkedLocations;
        std::mutex m_checkedMutex;

        // Killsanity (milestones/species). The tracker always runs, so kills before the slot connects are not
        // lost; IDs are only sent once slot data says killsanity >= 1 (m_killsanity: 0 = off or not yet known).
        // On connect with killsanity >= 1, everything earned so far is sent. SendLocation de-dups and persists.
        // Guarded by m_killMutex (pipe thread feeds events, poll thread sets killsanity).
        std::mutex m_killMutex;
        std::unique_ptr<KillTracker> m_killTracker;
        std::vector<int64_t> m_killEarned;
        int m_killsanity{ 0 };
        void FeedKillTracker(const AiEvent& ev);
    };

}  // namespace haloap