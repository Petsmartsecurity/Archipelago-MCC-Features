#include "ap_bridge.h"
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
    
    std::string m_mission;
    
    void APBridge::SendCompletionState() {
        if (!m_sendToDll) return;
        std::lock_guard<std::mutex> sendLock(m_sendStateMutex);
        std::set<int64_t> checked;
        {
            std::lock_guard<std::mutex> lock(m_checkedMutex);
            checked = m_checkedLocations;
        }
        std::vector<std::string> msgs;
        {
            std::lock_guard<std::mutex> lock(m_progressMutex);
            msgs = m_progress.DllMessages(checked);
        }
        for (const auto& m : msgs) {
            m_sendToDll(m);
            std::cout << "[ap] Sent to DLL: " << m << "\n";
        }
    }

    APBridge::APBridge() : m_killTracker(std::make_unique<KillTracker>(GetKillTables())) {}

    // Caller holds m_killMutex. Sends the earned IDs that are locations of this slot; the rest (e.g. a milestone
    // above the seed's cap) are dropped. SendLocation only queues + persists, so it is safe from either thread.
    // Already-checked IDs (a reconnect re-emits everything earned) are skipped silently (QA 5.4 m3). Kill IDs never
    // change mission/skull completion, so the DLL isn't re-notified per ID. Logs no mission name: m_mission belongs
    // to the pipe thread and this also runs on the poll thread (QA 5.4 m1).
    void APBridge::SendKillChecks(const std::vector<int64_t>& earned) {
        for (int64_t id : earned) {
            if (m_slotLocations.count(id) == 0) continue;  // e.g. a milestone above this seed's cap
            {
                std::lock_guard<std::mutex> lock(m_checkedMutex);  // order: m_killMutex -> m_checkedMutex
                if (m_checkedLocations.count(id)) continue;
            }
            std::cout << "[ap] kill check earned: " << id << "\n";
            SendLocation(id, /*notifyDll=*/false);
        }
    }

    // Pipe thread. Feeds the tracker; it emits nothing until the slot options are known (SetConfig on connect).
    void APBridge::FeedKillTracker(const AiEvent& ev) {
        std::lock_guard<std::mutex> lock(m_killMutex);
        SendKillChecks(m_killTracker->OnEvent(ev));
    }

    // Pipe thread. Same as FeedKillTracker for a Flood infection-form death (PLAN 6.R).
    void APBridge::FeedSwarm(const SwarmEvent& ev) {
        std::lock_guard<std::mutex> lock(m_killMutex);
        SendKillChecks(m_killTracker->OnSwarm(ev));
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
        
        {
            std::lock_guard<std::mutex> lock(m_itemBufferMutex);
            std::cout << "[ap] replaying " << m_itemBuffer.size() << " buffered items to DLL\n";
            for (int64_t itemId : m_itemBuffer) {
                m_sendToDll("ITEM_RECIVED: " + std::to_string(itemId));
            }
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
            auto mission = GameProgress::ByCode(missionCode);
            if (!mission) {
                // 00a_introduction (The Heretic) is a cutscene: never a check
                std::cerr << "[ap] unknown mission code: " << missionCode << "\n";
                return true;  // we recognized the message type, just couldn't map it
            }
            bool wanted;
            {
                std::lock_guard<std::mutex> lock(m_progressMutex);
                wanted = m_progress.Wants(mission->game);
            }
            if (!wanted) return true;  // a game this seed does not play

            std::cout << "[ap] MISSION_COMPLETE: " << missionCode
                << " -> location " << mission->id
                << " (" << mission->name << ")\n";

            SendLocation(mission->id);

            std::optional<Game> goalGame;
            {
                std::lock_guard<std::mutex> lock(m_progressMutex);
                goalGame = m_progress.OnMissionComplete(*mission);
            }
            if (goalGame)
            {
                // Persisted as its own per-game flag: the goal mission's completion is never a slot location (Phase 2 QA M1).
                {
                    std::lock_guard<std::mutex> lock(m_pendingMutex);
                    m_pending.SetGoalReached(GameTag(*goalGame));
                }
                m_goalRequested.store(true);  // the poll thread sends GOAL once the slot is connected and every enabled game is done
            }
            return true;
        }
        if (message.rfind("H2: ", 0) == 0) {
            // Observe-only (ADR-004 decision 5): telemetry line, no other effect.
            if (m_telemetry) m_telemetry->H2(message.substr(4));
            return true;
        }
        if (message.rfind("SWARM: ", 0) == 0) {
            // Log-only (PLAN 5.7): telemetry line, never the kill tracker, locations or goal.
            if (auto sw = ParseSwarmEvent(message)) {
                if (m_telemetry) m_telemetry->Swarm(m_mission, *sw);
                FeedSwarm(*sw);  // infection-form checks (PLAN 6.R): only if the slot has kill_infection_interval > 0
            } else {
                if (m_telemetry) m_telemetry->Rejected();
                std::cerr << "[ap] malformed SWARM message ignored\n";
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
                // new run: per-run kill count resets; a Halo 2 mission has no kill table, so track nothing
                m_killTracker->OnMissionLoad(GameProgress::IsH2Code(m_mission) ? "" : m_mission);
            }
            std::cout << "[launcher] Map: " << m_mission << "\n";
            return true;
        }
        if (message.rfind("KILL: ", 0) == 0 || message.rfind("ERASE: ", 0) == 0)
        {
            // Telemetry always; AP checks only via the kill tracker, once the slot's kill options are known.
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
            FeedKillTracker(*ev);  // KILL and ERASE both count (QUESTIONS #9)
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
            if (auto h2 = GameProgress::H2Chapter(m_mission, *chapterIdx)) {
                bool wanted;
                {
                    std::lock_guard<std::mutex> lock(m_progressMutex);
                    wanted = m_progress.Wants(Game::H2);
                }
                std::cout << "[ap] H2 chapter " << m_mission << ":" << *chapterIdx << " " << h2->name << "\n";
                if (wanted) SendLocation(h2->id);
                return true;
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

    void APBridge::SendLocation(int64_t locationId, bool notifyDll) {
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
        if (notifyDll) SendCompletionState();
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
            std::set<std::string> flags;
            {
                std::lock_guard<std::mutex> lock(m_pendingMutex);
                flags = m_pending.Goals();
            }
            bool ready;
            {
                std::lock_guard<std::mutex> lock(m_progressMutex);
                ready = m_progress.GoalReached(flags);
            }
            if (ready) m_client->StatusUpdate(APClient::ClientStatus::GOAL);
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
                bool isCompletion = GameProgress::ByLocation(id).has_value();
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
            // A different seed than the last connect in this run: the old seed's checks don't apply (m_lastSeed is
            // written below, on this same thread).
            if (!m_lastSeed.empty() && m_client->get_seed() != m_lastSeed) {
                m_checkedLocations.clear();
                // Not replayed to a restarted game. What the running game already got stays unlocked there until MCC
                // restarts (the DLL has no reset message).
                std::lock_guard<std::mutex> itemLock(m_itemBufferMutex);
                m_itemBuffer.clear();
            }
            auto checked = m_client->get_checked_locations();
            for (int64_t locId : checked) {
                m_checkedLocations.insert(locId);
            }
        }
        
        std::vector<Game> earlyGoals;  // finals completed before the config was known
        {
            std::lock_guard<std::mutex> lock(m_progressMutex);
            earlyGoals = m_progress.Configure(slotData);
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
            for (Game g : earlyGoals) m_pending.SetGoalReached(GameTag(g));
            m_goalRequested.store(true);  // re-evaluated (and, if reached, resent: idempotent on the server) on every connect
        }
        // Kill options from slot data (after Bind so queued kill checks land in this seed's pending file).
        // Missing keys = 0 / off (a seed from an older apworld has no kill checks).
        {
            int interval = 0;
            int infection = 0;
            bool speciesOn = false;
            if (slotData.contains("kill_milestone_interval") && slotData["kill_milestone_interval"].is_number_integer()) {
                interval = slotData["kill_milestone_interval"].get<int>();
            }
            if (slotData.contains("kill_infection_interval") && slotData["kill_infection_interval"].is_number_integer()) {
                infection = slotData["kill_infection_interval"].get<int>();
            }
            if (slotData.contains("kill_species_checks") && slotData["kill_species_checks"].is_number_integer()) {
                speciesOn = slotData["kill_species_checks"].get<int>() != 0;
            }
            std::set<int64_t> slotLocs = m_client->get_checked_locations();  // poll thread: m_client is ours
            for (int64_t id : m_client->get_missing_locations()) slotLocs.insert(id);
            std::string seed = m_client->get_seed();
            std::lock_guard<std::mutex> lock(m_killMutex);
            // A different seed than the last connect: kills made for the old seed are not credited to the new one
            // (QA 5.4 m2). Kills made before the first connect still count for that first seed.
            if (!m_lastSeed.empty() && seed != m_lastSeed) m_killTracker->ResetHistory();
            m_lastSeed = seed;
            m_slotLocations = std::move(slotLocs);
            std::cout << "[ap] kill_milestone_interval = " << interval << ", kill_infection_interval = " << infection << ", kill_species_checks = " << speciesOn << "\n";
            // Everything earned so far under these settings (kills before the slot connected) is sent now.
            SendKillChecks(m_killTracker->SetConfig(interval, speciesOn, infection));
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