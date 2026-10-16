#include "chargelink/util/time.hpp"

#include <chrono>
#include <ctime>

namespace chargelink::util {

    std::string now_utc_iso8601() {
        const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &t);  // thread-safe variant on Windows
#else
        gmtime_r(&t, &tm);  // thread-safe variant on POSIX
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
        return buf;
    }

}  // namespace chargelink::util