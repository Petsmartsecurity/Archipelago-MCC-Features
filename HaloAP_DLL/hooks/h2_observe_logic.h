#pragma once
// Pure logic of the Halo 2 observe-only hook set (hooks/h2_observe.cpp): no game memory, no MinHook, no Windows,
// so mcc-ap/tests/test_h2_observe.cpp can run it on fake buffers.
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace haloap {
namespace h2 {

    constexpr uint32_t kHeaderMagic = 0x68656164;  // 'head' (as the first dword of a cache file header)

    // ---- message building -------------------------------------------------------------------------------------
    constexpr size_t kMsgMax = 400;  // whole message including the terminator

    // Bounded, stack-only message builder ("H2: ev=<name> n=<u> tid=<u> k=v ..."). No heap, no statics: re-entrant.
    class Msg {
    public:
        Msg(const char* ev, uint32_t n, uint32_t tid) { Add("H2: ev=%s n=%u tid=%u", ev, n, tid); }

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

        // Last line of defence: the launcher forwards this as text, so nothing outside printable ASCII may leave.
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

    // ---- sanitising -------------------------------------------------------------------------------------------
    inline char SafeChar(unsigned c) { return (c >= 0x21 && c <= 0x7E) ? static_cast<char>(c) : '_'; }

    // Copies at most srcMax chars (stops at NUL) into out[cap], replacing everything outside 0x21..0x7E (space too)
    // with '_'. Returns the length written; out is always terminated.
    inline size_t SanitizeAscii(const char* src, size_t srcMax, char* out, size_t cap) {
        if (!cap) return 0;
        size_t n = 0;
        for (; src && n < srcMax && n + 1 < cap && src[n]; n++) out[n] = SafeChar(static_cast<unsigned char>(src[n]));
        out[n] = 0;
        return n;
    }

    // Last path component of a UTF-16 string (separator '\\' or '/'), sanitised; non-ASCII becomes '_'.
    // maxChars bounds the read (the field is 260 wchar).
    inline size_t SanitizeWidePathTail(const uint16_t* w, size_t maxChars, char* out, size_t cap) {
        if (!cap) return 0;
        out[0] = 0;
        if (!w) return 0;
        size_t len = 0, tail = 0;
        while (len < maxChars && w[len]) {
            if (w[len] == '\\' || w[len] == '/') tail = len + 1;
            len++;
        }
        size_t n = 0;
        for (size_t i = tail; i < len && n + 1 < cap; i++) out[n++] = SafeChar(w[i]);
        out[n] = 0;
        return n;
    }

    // Mission code from the cache header name field (char[32]): 1..31 printable ASCII chars (0x21..0x7E) and a NUL
    // inside the field. Returns false (out = "") if not.
    inline bool ValidCode(const char* name32, char* out /* >= 32 */) {
        out[0] = 0;
        if (!name32) return false;
        size_t n = 0;
        while (n < 32 && name32[n]) {
            auto c = static_cast<unsigned char>(name32[n]);
            if (c < 0x21 || c > 0x7E) return false;
            n++;
        }
        if (n == 0 || n > 31) return false;
        memcpy(out, name32, n);
        out[n] = 0;
        return true;
    }

    // ---- classification ---------------------------------------------------------------------------------------
    // Addresses are absolute return addresses; 0 = "unknown / not resolved".
    enum class StartKind { Load, Reset, Other };
    inline StartKind ClassifyStart(uintptr_t ret, uintptr_t loadRet, uintptr_t resetRet) {
        if (loadRet && ret == loadRet) return StartKind::Load;
        if (resetRet && ret == resetRet) return StartKind::Reset;
        return StartKind::Other;
    }
    inline const char* StartKindName(StartKind k) {
        return k == StartKind::Load ? "load" : k == StartKind::Reset ? "reset" : "other";
    }

    // QA 8.H2.7a M5: actor_died called from inside unit_die = kill, any other call site = erase.
    // killRet == 0 means the unit_die/actor_died link could not be verified: never claim a kill.
    enum class DeathKind { Kill, Erase, Unknown };
    inline DeathKind ClassifyDeath(uintptr_t ret, uintptr_t killRet) {
        if (!killRet) return DeathKind::Unknown;
        return ret == killRet ? DeathKind::Kill : DeathKind::Erase;
    }
    inline const char* DeathKindName(DeathKind k) {
        return k == DeathKind::Kill ? "kill" : k == DeathKind::Erase ? "erase" : "unknown";
    }

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

    // ---- data arrays (QA 8.H2.7a m6) --------------------------------------------------------------------------
    // Header: name[32], +0x20 max count, +0x24 element size, +0x2C signature, +0x48 offset of element 0.
    // Element: +0 salt (u16) == handle >> 16. The caller wraps this in SEH when `arr` is game memory.
    constexpr uint32_t kDatumSignature = 0x64407440;
    constexpr size_t kDatumMaxOff = 0x20, kDatumSizeOff = 0x24, kDatumSigOff = 0x2C, kDatumDataOff = 0x48;

    inline const uint8_t* DatumElement(const uint8_t* arr, uint32_t handle, uint32_t elemSize) {
        if (!arr || handle == 0xFFFFFFFFu) return nullptr;
        uint32_t sig, size;
        int32_t maxCount, dataOff;
        memcpy(&sig, arr + kDatumSigOff, 4);
        memcpy(&size, arr + kDatumSizeOff, 4);
        memcpy(&maxCount, arr + kDatumMaxOff, 4);
        memcpy(&dataOff, arr + kDatumDataOff, 4);
        if (sig != kDatumSignature || size != elemSize) return nullptr;
        uint32_t idx = handle & 0xFFFF;
        if (maxCount <= 0 || idx >= static_cast<uint32_t>(maxCount)) return nullptr;
        if (dataOff <= 0 || dataOff > 0x10000000) return nullptr;  // 0 = no data
        const uint8_t* e = arr + dataOff + static_cast<size_t>(idx) * elemSize;
        uint16_t salt;
        memcpy(&salt, e, 2);
        if (salt != (handle >> 16)) return nullptr;
        return e;
    }

}  // namespace h2
}  // namespace haloap
