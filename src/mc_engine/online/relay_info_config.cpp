#include "relay_info.h"

#include <cstdlib>

#include "settings/larecomp_settings.h"

namespace larecomp::relay_info {

void ConfiguredServer(std::string& host, uint16_t& port) {
    host = larecomp::cvar::GetFlagByName("mcla_online_server");
    const int cfg = std::atoi(larecomp::cvar::GetFlagByName("mcla_online_port").c_str());
    port = (cfg > 0 && cfg < 65536) ? uint16_t(cfg) : uint16_t(31000);
    if (const size_t colon = host.rfind(':'); colon != std::string::npos) {
        const int p = std::atoi(host.c_str() + colon + 1);
        if (p > 0 && p < 65536) port = uint16_t(p);
        host.resize(colon);
    }
    if (host.empty()) host = "127.0.0.1";
}

}  // namespace larecomp::relay_info
