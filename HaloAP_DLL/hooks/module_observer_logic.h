#pragma once
// Pure logic shared by every game-module observer (hooks/module_observer.{h,cpp}; today Halo 2 and Halo 3): no game memory,
// no MinHook, no Windows, so mcc-ap/tests/test_module_observer.cpp can run it on fake buffers.
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace haloap {
namespace obs {

    // ---- telemetry line --------------------------------------------------------------------------------------------
    constexpr size_t kMsgMax = 400;  // whole message including the terminator

    // Bounded, stack-only message builder ("<TAG>: ev=<name> n=<u> tid=<u> k=v ..."; tag "H2" / "H3"). No heap, no statics:
    // re-entrant. The launcher forwards the line as text, so Finish() maps everything outside printable ASCII to '_'.
    class Msg {
    public:
        Msg(const char* tag, const char* ev, uint32_t n, uint32_t tid) { Add("%s: ev=%s n=%u tid=%u", tag, ev, n, tid); }

        // Appends " " + formatted text (callers pass "k=v" with already sanitised values).
        void Add(const char* fmt, ...) {
            if (m_len >= kMsgMax - 1) { m_truncated = true; return; }
            if (m_len) m_buf[m_len++] = ' ';
            va_list ap;
            va_start(ap, fmt);
            int r = vsnprintf(m_buf + m_len, kMsgMax - m_len, fmt, ap);
            va_end(ap);
            if (r < 0) { m_buf[m_len] = 0; return; }
            if (static_cast<size_t>(r) >= kMsgMax - m_len) { m_len = kMsgMax - 1; m_truncated = true; }
            else m_len += static_cast<size_t>(r);
            m_buf[m_len] = 0;
        }

        const char* Finish() {
            for (size_t i = 0; i < m_len; i++) {
                auto c = static_cast<unsigned char>(m_buf[i]);
                if (c < 0x20 || c > 0x7E) m_buf[i] = '_';
            }
            return m_buf;
        }
        size_t size() const { return m_len; }
        bool truncated() const { return m_truncated; }

    private:
        char m_buf[kMsgMax] = {};
        size_t m_len = 0;
        bool m_truncated = false;
    };

    // ---- module identity and lifecycle decisions ---------------------------------------------------------------------
    struct ModId {
        uintptr_t base = 0;
        uint32_t stamp = 0, size = 0;
        bool operator==(const ModId& o) const { return base == o.base && stamp == o.stamp && size == o.size; }
    };

    // What the ~2 s integrity check does (QA 8.H2.7b m2). enabled = hooks we think are installed, intact = of those, how
    // many still start with MinHook's jump. A reload looks like "identity same, no prologue is ours any more"; a foreign
    // patch on one function looks like "some are intact": drop only those, keep anchors and the rest.
    enum class Reattach { None, DropChanged, Full };
    inline Reattach DecideReattach(bool identityChanged, int enabled, int intact) {
        if (identityChanged) return Reattach::Full;
        if (enabled <= 0) return Reattach::None;
        if (intact <= 0) return Reattach::Full;
        return intact < enabled ? Reattach::DropChanged : Reattach::None;
    }

    // The worker's per-slice comparison of the loaded module handle with the one we are attached to (both as integers).
    enum class Presence { Absent, Same, Attach, Replaced, Unloaded };
    inline Presence ClassifyPresence(uintptr_t attached, uintptr_t current) {
        if (current == attached) return current ? Presence::Same : Presence::Absent;
        if (!attached) return Presence::Attach;
        return current ? Presence::Replaced : Presence::Unloaded;
    }

    // Retry schedule for whatever is still missing after the first pass (QA 8.H2.7b m3): stage 0 -> one retry at ~2 s after
    // attach, stage 1 -> another at ~10 s, then none.
    constexpr int kRetryStages = 2;
    inline bool RetryDue(int stage, uint64_t elapsedMs) {
        if (stage < 0 || stage >= kRetryStages) return false;
        return elapsedMs >= (stage == 0 ? 2000ULL : 10000ULL);
    }

    // Integrity check cadence: every 20th 100 ms slice (~2 s). `counter` is the worker's slice counter, reset when due.
    constexpr int kIntegrityEverySlices = 20;
    inline bool IntegrityDue(int* counter) {
        if (++*counter < kIntegrityEverySlices) return false;
        *counter = 0;
        return true;
    }

    // Appends `s` to the comma separated list `list[cap]` (dropped silently when it does not fit).
    inline void AppendName(char* list, size_t cap, const char* s) {
        size_t n = strlen(list);
        if (n + 1 >= cap) return;
        snprintf(list + n, cap - n, "%s%s", n ? "," : "", s);
    }

    // Shell message coalescing (QA 8.H2.7b n5): after an id repeats, only every kShellRepeatEvery-th repeat is sent.
    constexpr uint32_t kShellRepeatEvery = 64;
    inline bool ShellRepeatDue(uint32_t repeatCount) { return repeatCount && repeatCount % kShellRepeatEvery == 0; }

    // ---- code scanning ----------------------------------------------------------------------------------------------
    // E8 rel32 call site at `site` (address of the E8 byte) targets `target`? Reads site[0..4] only.
    inline bool CallTargets(const uint8_t* site, const uint8_t* target) {
        if (site[0] != 0xE8) return false;
        int32_t rel;
        memcpy(&rel, site + 1, sizeof(rel));
        return site + 5 + rel == target;
    }

    // Collects the addresses of E8 rel32 calls to `target` inside [code, code+len); returns how many exist
    // (stores up to cap of them). Used for "game_start has exactly two callers".
    inline size_t FindCallSites(const uint8_t* code, size_t len, const uint8_t* target, const uint8_t** out, size_t cap) {
        size_t n = 0;
        if (len < 5) return 0;
        const uint8_t* p = code;
        const uint8_t* end = code + len - 4;  // last byte at which a 5-byte call starts is code+len-5
        while (p < end) {
            p = static_cast<const uint8_t*>(memchr(p, 0xE8, static_cast<size_t>(end - p)));
            if (!p) break;
            if (CallTargets(p, target)) {
                if (n < cap) out[n] = p;
                n++;
            }
            p++;
        }
        return n;
    }

}  // namespace obs
}  // namespace haloap
