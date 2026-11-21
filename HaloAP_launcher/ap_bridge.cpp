#include "ap_bridge.h"
#include "data/mission_map.h"
#include "data/chapter_map.h"
#include "shared/common.h"
#include "data/kill_map.h"
#include "pipe_protocol.h"
#include <atomic>
#include <iostream>
#include <vector>
#include <set>
#include <sstream>
#include <string>


namespace haloap {
    
    static const int64_t COMPLETION_LOCATIONS[] = {
        101000, 102000, 103000, 104000, 105000,
        106000, 107000, 108000, 109000, 110000
    };
    
    std::unordered_map<std::string, int> MISSION_NAME_TO_INDEX = {
    {"The Pillar of Autumn", 0},
    {"Halo (CE)", 1},
    {"The Truth and Reconciliation", 2},
    {"The Silent Cartographer", 3},
    {"Assault on the Control Room", 4},
    {"343 Guilty Spark", 5},
    {"The Library", 6},
    {"Two Betrayals", 7},
    {"Keyes", 8},
    {"The Maw", 9},
    };  
    std::atomic<int> m_finalMission{ 9 };
    
    std::unordered_map<std::string, int> MISSION_CODE_TO_INDEX = {
    {"a10", 0},
    {"a30", 1},
    {"a50", 2},
    {"b30", 3},
    {"b40", 4},
    {"c10", 5},
    {"c20", 6},
    {"c40", 7},
    {"d20", 8},
    {"d40", 9},
    };
    
    std::string m_mission;
    
    void APBridge::SendCompletionState() {
        if (!m_sendToDll) return;
        std::lock_guard<std::mutex> lock(m_checkedMutex);
    
        std::string msg = "COMPLETED:";
        bool first = true;
        for (int i = 0; i < 10; i++) {
            if (m_checkedLocations.count(COMPLETION_LOCATIONS[i])) {
                if (!first) msg += ",";
                msg += std::to_string(i);
                first = false;
            }
        }
        m_sendToDll(msg);
        std::cout << "[ap] Sent completion state: " << msg << "\n";
        
        m_sendToDll("FINAL_MISSION:" + std::to_string(m_finalMission));
        std::cout << "[ap] Sent final mission: " << m_finalMission << "\n";
    }

    APBridge::APBridge() : m_killTracker(std::make_unique<KillTracker>(GetKillTables())) {}

    // Pipe thread. Feeds the tracker and sends what it earns (if killsanity is known to be on).
    void APBridge::FeedKillTracker(const AiEvent& ev) {
        std::lock_guard<std::mutex> lock(m_killMutex);
        for (int64_t id : m_killTracker->OnEvent(ev)) {
            m_killEarned.push_back(id);
            std::cout << "[ap] kill check earned: " << id << " (" << m_mission << ", " << m_killTracker->Count()
                      << " kills)" << (m_killsanity >= 1 ? "\n" : " (held; killsanity not on/known yet)\n");
            if (m_killsanity >= 1) SendLocation(id);  // only queues + persists; safe from the pipe thread
        }
    }

    APBridge::~APBridge() {
        Stop();
    }
    
    void APBridge::ReplayBufferedItems() {
        if (!m_sendToDll) return;
        if (m_skullsanityTier >= 0)
        {
            m_sendToDll("SKULLSANITY: "+ std::to_string(m_skullsanityTier));
        }
        
        std::lock_guard<std::mutex> lock(m_itemBufferMutex);
        std::cout << "[ap] replaying " << m_itemBuffer.size() << " buffered items to DLL\n";
        for (int64_t itemId : m_itemBuffer) {
            m_sendToDll("ITEM_RECIVED: " + std::to_string(itemId));
        }
        // Also send current completion state
        SendCompletionState();
    }

    bool APBridge::Start(const std::string& serverUri,
        const std::string& game,
        const std::string& slot,
        const std::string& password) {
        m_game = game;
        m_slot = slot;
        m_password = password;

        {
            // Memory only until the slot connects: the pending file is keyed by seed, which we don't know yet.
            std::lock_guard<std::mutex> lock(m_pendingMutex);
            m_pending = PendingChecks();
        }

        try {
            // Construct APClient. The UUID should be stable across runs for a user
            // so the server recognizes them, but for alpha we generate one per run.
            std::string uuid = "";
            m_client = std::make_unique<APClient>(uuid, game, serverUri);
        }
        catch (const std::exception& e) {
            std::cerr << "[ap] APClient construction failed: " << e.what() << "\n";
            return false;
        }

        // Register callbacks. Captures `this`; lifetime is fine because the client
        // is owned by us and callbacks only fire during Poll().
        m_client->set_socket_connected_handler([this]() { OnSocketConnected(); });
        m_client->set_socket_error_handler([this](const std::string& err) { OnSocketError(err); });
        m_client->set_socket_disconnected_handler([this]() { OnSocketDisconnected(); });
        m_client->set_room_info_handler([this]() { OnRoomInfo(); });
        m_client->set_slot_connected_handler([this](const nlohmann::json& data) { OnSlotConnected(data); });
        m_client->set_slot_refused_handler([this](const std::list<std::string>& reasons) { OnSlotRefused(reasons); });
        m_client->set_items_received_handler([this](const std::list<APClient::NetworkItem>& items) { OnItemsReceived(items); });
        m_client->set_print_json_handler([this](const APClient::PrintJSONArgs& args) { OnPrintJson(args); });
        m_client->set_location_checked_handler([this](const std::list<int64_t>& locs) { OnLocationChecked(locs); });
        // Without this, apclientpp marks our own checks done locally and never reports the server's echo,
        // so pending checks would never be confirmed during a session (Phase 2 QA B1).
        m_client->set_receive_own_locations(true);

        std::cout << "[ap] connecting to " << serverUri << " as slot '" << slot << "'...\n";
        return true;
    }

    void APBridge::Poll() {
        if (m_stopped.load()) return;
        if (m_client) {
            m_client->poll();
            if (m_flushRequested.load() || m_goalRequested.load()) FlushPending();
        }
    }

    void APBridge::Stop() {
        if (m_stopped.exchange(true)) return;
        NotePipeState(false);
        m_client.reset();
        m_socketConnected.store(false);
        m_slotConnected.store(false);
    }

    bool APBridge::HandleDllMessage(const std::string& message) {
        const std::string missionCompPrefix = "MISSION_COMPLETE: ";
        if (message.rfind(missionCompPrefix, 0) == 0) {
            auto parsedCode = ParseMissionComplete(message);
            if (!parsedCode) {
                std::cerr << "[ap] malformed MISSION_COMPLETE message ignored\n";
                return true;
            }
            const std::string& missionCode = *parsedCode;
            if (m_telemetry) m_telemetry->MissionEnd(missionCode, "won");
            m_mission.clear();  // events until the next valid MISSION_LOAD have no mission
            {
                std::lock_guard<std::mutex> lock(m_killMutex);
                m_killTracker->OnMissionLoad("");
            }
            const auto& map = GetMissionIdMap();
            auto it = map.find(missionCode);
            if (it == map.end()) {
                std::cerr << "[ap] unknown mission code: " << missionCode << "\n";
                return true;  // we recognized the message type, just couldn't map it
            }

            int64_t locationId = it->second;
            std::cout << "[ap] MISSION_COMPLETE: " << missionCode
                << " -> location " << locationId
                << " (" << GetMissionDisplayName(locationId) << ")\n";

            SendLocation(locationId);
        
            auto idx = MISSION_CODE_TO_INDEX.find(missionCode);
            if (idx != MISSION_CODE_TO_INDEX.end() && idx->second == m_finalMission.load())
            {
                // Persisted as its own flag: the goal mission's completion is never a slot location (Phase 2 QA M1).
                {
                    std::lock_guard<std::mutex> lock(m_pendingMutex);
                    m_pending.SetGoalReached();
                }
                m_goalRequested.store(true);  // sent by the poll thread once the slot is connected
            }
        
            return true;
        }
        if (message.rfind("HELLO", 0) == 0) {
            // The DLL sends HELLO once when its pipe connects; a second HELLO means it reconnected.
            NotePipeState(true);
            return false;  // keep existing "not handled" behaviour for HELLO
        }
        const std::string missionPrefix = "MISSION_LOAD: ";
        if (message.rfind(missionPrefix, 0) == 0)
        {
            // Mission code comes from a path like "levels\d40\d40" (parsed in pipe_protocol.cpp).
            if (auto code = ParseMissionLoad(message)) {
                m_mission = *code;
                if (m_telemetry) m_telemetry->MissionLoad(m_mission);
            } else {
                m_mission.clear();  // don't tag later events with the previous mission
            }
            {
                std::lock_guard<std::mutex> lock(m_killMutex);
                m_killTracker->OnMissionLoad(m_mission);  // new run: per-run kill count resets
            }
            std::cout << "[launcher] Map: " << m_mission << "\n";
            return true;
        }
        if (message.rfind("KILL: ", 0) == 0 || message.rfind("ERASE: ", 0) == 0)
        {
            // Telemetry always; AP checks only via the kill tracker when killsanity >= 1.
            auto ev = ParseAiEvent(message);
            if (!ev) {
                if (m_telemetry) m_telemetry->Rejected();
                std::cerr << "[ap] malformed KILL/ERASE message ignored\n";
                return true;
            }
            if (m_telemetry) {
                if (ev->kill) m_telemetry->Death(m_mission, *ev);
                else m_telemetry->Erase(m_mission, *ev);
            }
            FeedKillTracker(*ev);  // ignores ERASE
            return true;
        }
        const std::string chapterPrefix = "CHAPTER:";
        if (message.rfind(chapterPrefix, 0) == 0)
        {
            auto chapterIdx = ParseChapter(message);
            if (!chapterIdx) {
                std::cerr << "[ap] malformed CHAPTER message ignored\n";
                return true;
            }
            if (m_mission.empty())
            {
                std::cerr << "[ap] didn't find mission, return to menu and restart mission\n";
            }
            std::string chapterCode = m_mission + ":" + std::to_string(*chapterIdx);
            std::cout << "[ap] chapter " << chapterCode << "\n";
            
            auto& chapters = haloap::GetChapterMap();
            auto chapterData = chapters.find(chapterCode);
            if (chapterData != chapters.end()) {
                std::cout << "[ap] Chapter: " << chapterData->second.name << "\n";
                SendLocation(chapterData->second.locationId);
            } else {
                std::cout << "[ap] Unknown chapter: " << chapterCode << "\n";
            }
            return true;
        }
        
        const std::string locationPrefix = "LOCATION_CHECKED: ";
        if (message.rfind(locationPrefix, 0) == 0)
        {
            auto locationId = ParseLocationChecked(message);
            if (!locationId) {
                std::cerr << "[ap] malformed LOCATION_CHECKED message ignored\n";
                return true;
            }
            std::cout << "[ap] skull location checked: " << *locationId << "\n";
            SendLocation(*locationId);
            return true;
        }
        
        
        return false;
    }

    void APBridge::SendLocation(int64_t locationId) {
        // Queue (and persist) first; the poll thread sends it once the slot is connected.
        // Runs on the pipe thread, so it never touches m_client.
        {
            std::lock_guard<std::mutex> lock(m_pendingMutex);
            if (!m_pending.Add(locationId)) {
                std::cout << "[ap] location " << locationId << " already pending or confirmed, skipping\n";
                return;
            }
            if (m_pending.LastSaveFailed() && !m_saveFailureLogged.exchange(true)) {
                std::cerr << "[ap] WARNING: could not write " << m_pending.Path()
                          << "; unsent checks will be lost if the launcher closes before reconnecting\n";
            }
        }
        m_flushRequested.store(true);
        if (!m_slotConnected.load()) {
            std::cout << "[ap] not connected; queued location " << locationId << " until slot connects\n";
        }

        // Track completion and notify DLL
        {
            std::lock_guard<std::mutex> lock(m_checkedMutex);
            m_checkedLocations.insert(locationId);
        }
        SendCompletionState();
    }

    // Poll thread only. Sends every pending location the server hasn't confirmed or been sent on this connection.
    void APBridge::FlushPending() {
        if (!m_slotConnected.load() || !m_client) return;
        m_flushRequested.store(false);

        std::list<int64_t> toSend;
        {
            std::lock_guard<std::mutex> lock(m_pendingMutex);
            for (int64_t id : m_pending.Pending()) {
                if (!m_inFlight.count(id)) toSend.push_back(id);
            }
        }
        if (!toSend.empty()) {
            m_client->LocationChecks(toSend);
            for (int64_t id : toSend) {
                m_inFlight.insert(id);
                std::cout << "[ap] sent location check: " << id << "\n";
            }
        }
        if (m_goalRequested.exchange(false)) {
            m_client->StatusUpdate(APClient::ClientStatus::GOAL);
        }
    }

    // Server reports locations as checked (after slot connect, and via RoomUpdate).
    void APBridge::OnLocationChecked(const std::list<int64_t>& locations) {
        {
            std::lock_guard<std::mutex> lock(m_pendingMutex);
            for (int64_t id : locations) m_pending.Confirm(id);
        }
        bool completionChanged = false;
        {
            std::lock_guard<std::mutex> lock(m_checkedMutex);
            for (int64_t id : locations) {
                bool isCompletion = false;
                for (int64_t c : COMPLETION_LOCATIONS) isCompletion = isCompletion || (c == id);
                if (m_checkedLocations.insert(id).second && isCompletion) completionChanged = true;
            }
        }
        for (int64_t id : locations) m_inFlight.erase(id);
        if (completionChanged) SendCompletionState();
    }

    // -------- callbacks ---------

    void APBridge::OnSocketConnected() {
        m_socketConnected.store(true);
        std::cout << "[ap] socket connected\n";
    }

    void APBridge::OnSocketError(const std::string& error) {
        std::cerr << "[ap] socket error: " << error << "\n";
    }

    void APBridge::OnSocketDisconnected() {
        m_socketConnected.store(false);
        m_slotConnected.store(false);
        m_inFlight.clear();  // anything unconfirmed gets resent after reconnect
        std::cout << "[ap] socket disconnected (will auto-reconnect)\n";
    }

    void APBridge::OnRoomInfo() {
        std::cout << "[ap] room info received; attempting slot connect...\n";
        // ConnectSlot params: slot name, password, items-handling flags, tags, version
        // Items handling 0b111 = all items (local, starting inventory, world items).
        std::list<std::string> tags;  // no special tags for now
        m_client->ConnectSlot(m_slot, m_password, 0b111, tags, { 1, 2, 0 });
    }

    void APBridge::OnSlotConnected(const nlohmann::json& slotData) {
        m_inFlight.clear();
        std::cout << "[ap] *** slot connected ***\n";
        if (!slotData.empty()) {
            std::cout << "[ap] slot data: " << slotData.dump() << "\n";
        }
    
        // Load already-checked locations from server
        {
            std::lock_guard<std::mutex> lock(m_checkedMutex);
            auto checked = m_client->get_checked_locations();
            for (int64_t locId : checked) {
                m_checkedLocations.insert(locId);
            }
        }
        
        if (slotData.contains("final_mission"))
        {
            m_finalMission = MISSION_NAME_TO_INDEX[slotData.at("final_mission").get<std::string>()];
        }
        
        if (slotData.contains("skullsanity"))
        {
            m_skullsanityTier = slotData["skullsanity"].get<int>();
        }

        // Bind to this seed's pending file, then reconcile with the server (logic + tests in pending_checks.cpp).
        {
            std::lock_guard<std::mutex> lock(m_pendingMutex);
            if (!m_stateDir.empty()) {
                m_pending.Bind(m_stateDir + PendingFileName(m_client->get_seed(), m_slot));
            }
            ReconcileResult r = m_pending.Reconcile(m_client->get_checked_locations(), m_client->get_missing_locations());
            if (r.dropped) {
                std::cout << "[ap] dropped " << r.dropped << " queued check(s) that are not locations in this slot\n";
            }
            if (!r.toSend.empty()) {
                std::cout << "[ap] " << r.toSend.size() << " unsent location check(s) to flush\n";
            }
            if (r.sendGoal) {
                m_goalRequested.store(true);  // idempotent on the server, so resent on every connect
            }
        }
        // Killsanity from slot data (after Bind so queued kill checks land in this seed's pending file).
        {
            int ks = 0;
            if (slotData.contains("killsanity") && slotData["killsanity"].is_number_integer()) {
                ks = slotData["killsanity"].get<int>();
            }
            std::lock_guard<std::mutex> lock(m_killMutex);
            m_killsanity = ks;
            std::cout << "[ap] killsanity = " << ks << "\n";
            if (ks >= 1) {
                // Kills made before the slot connected (or before this data arrived) are sent now.
                for (int64_t id : m_killEarned) SendLocation(id);
            }
        }
        m_slotConnected.store(true);
        m_flushRequested.store(true);
        FlushPending();

        SendCompletionState();
    }

    void APBridge::OnSlotRefused(const std::list<std::string>& reasons) {
        std::cerr << "[ap] slot refused:\n";
        for (const auto& r : reasons) {
            std::cerr << "  - " << r << "\n";
        }
    }

    void APBridge::OnItemsReceived(const std::list<APClient::NetworkItem>& items) {
        for (const auto& item : items) {
            std::string itemName = m_client->get_item_name(item.item, m_game);
            std::cout << "[ap] received item: " << itemName
                << " (id " << item.item << ")\n";

            // Buffer the item
            {
                std::lock_guard<std::mutex> lock(m_itemBufferMutex);
                m_itemBuffer.push_back(item.item);
            }

            // Forward to DLL if connected
            if (m_sendToDll) {
                m_sendToDll("ITEM_RECIVED: " + std::to_string(item.item));
            }
        }
    }

    void APBridge::OnPrintJson(const APClient::PrintJSONArgs& args) {
        // Render the message as plain text.
        std::string text;
        for (const auto& part : args.data) {
            text += part.text;
        }
        std::cout << "[ap chat] " << text << "\n";
    }

}  // namespace haloap