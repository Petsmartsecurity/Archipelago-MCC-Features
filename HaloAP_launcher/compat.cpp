#include "compat.h"

#include <cctype>

namespace haloap {

    std::string NormalizeServerAddress(const std::string& typed) {
        size_t b = 0, e = typed.size();
        while (b < e && std::isspace(static_cast<unsigned char>(typed[b]))) ++b;
        while (e > b && std::isspace(static_cast<unsigned char>(typed[e - 1]))) --e;
        std::string s = typed.substr(b, e - b);
        if (s.empty() || s.find("://") != std::string::npos) return s;
        if (s.find("localhost") != std::string::npos || s.find("127.0.0.1") != std::string::npos ||
            s.find("[::1]") != std::string::npos) {
            return "ws://" + s;
        }
        return s;
    }

    SeedCompat CheckSeedCompat(int version) {
        if (version < 0) return SeedCompat::Missing;
        return version > kKillsanityVersion ? SeedCompat::Newer : SeedCompat::Ok;
    }

}  // namespace haloap
