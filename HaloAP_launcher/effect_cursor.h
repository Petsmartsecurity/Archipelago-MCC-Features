#pragma once
// CE effect items on the launcher side (ADR-005, research 3.5). AP replays EVERY received item on each connect; mission and
// skull items are idempotent, effects are not. This keeps, per seed + slot (file haloap_effects_<hash>.txt next to the
// pending file):
//   - the cursor: the highest AP item index already taken over. An effect item with index <= cursor is never forwarded again.
//   - the in-flight list: items taken over but not yet reported applied by the DLL (EFFECT_APPLIED / EFFECT_SKIPPED). They are
//     re-sent when the DLL first connects in a launcher run (ReplayBufferedItems), and the DLL ignores duplicates. Traps are acked
//     BEFORE the post (at most once), good effects after it (at least once): a lost ack can repeat a good effect, never a trap.
// Pure C++20, no AP dependencies. NOT thread-safe: the owner locks.
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace haloap {

    // CE effect item ids: 80000 + game*1000 + k (apworld items.py); only the CE block is handled.
    constexpr int64_t kEffectIdFirst = 81000;
    constexpr int64_t kEffectIdLast = 81999;
    inline bool IsEffectItemId(int64_t id) { return id >= kEffectIdFirst && id <= kEffectIdLast; }

    // "haloap_effects_<16 hex>.txt" (same seed + slot hash as PendingFileName).
    std::string EffectFileName(const std::string& seed, const std::string& slot);

    // Launcher -> DLL message: "EFFECT: <item id> <index>".
    std::string EffectMessage(int64_t id, int index);

    struct EffectItem {
        int index;
        int64_t id;
    };

    class EffectCursor {
    public:
        explicit EffectCursor(std::string path = {});

        // Switches to this seed's file (loads it). Same path again = no-op; a different path drops the in-memory state first.
        void Bind(const std::string& path);

        // An effect item arrived from AP. Returns true if it is new (index above the cursor) and should be forwarded now;
        // false for a replay or an index < 0 (starting inventory / unsynced: the apworld never precollects effects, so it is
        // ignored, and the caller logs it).
        bool OnItem(int64_t id, int index);

        // The DLL reported the item applied (or skipped). True if it was in flight.
        bool OnDone(int64_t id, int index);

        std::vector<EffectItem> InFlight() const;  // ordered by index
        int Cursor() const { return m_cursor; }
        const std::string& Path() const { return m_path; }
        bool Save() const;

    private:
        void Load();
        std::string m_path;
        int m_cursor = -1;
        std::map<int, int64_t> m_inFlight;  // index -> item id
    };

}  // namespace haloap
