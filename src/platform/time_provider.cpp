#include "platform/time_provider.h"

#include <ctime>

namespace pzip {
namespace platform {

void TimeProvider::fillDosDateTime(uint32_t* outTime, uint32_t* outDate) const {
    if (outTime == NULL || outDate == NULL) {
        return;
    }

    const std::time_t cftime = std::time(NULL);
    std::tm tmv;
#if defined(_WIN32)
    localtime_s(&tmv, &cftime);
#else
    localtime_r(&cftime, &tmv);
#endif

    const uint16_t dosTime = static_cast<uint16_t>(
        ((tmv.tm_hour & 0x1F) << 11) | ((tmv.tm_min & 0x3F) << 5) | ((tmv.tm_sec / 2) & 0x1F));
    const int year = tmv.tm_year + 1900;
    const uint16_t dosDate = static_cast<uint16_t>(((year < 1980 ? 1980 : year) - 1980) << 9 |
                                                   ((tmv.tm_mon + 1) << 5) | tmv.tm_mday);
    *outTime = dosTime;
    *outDate = dosDate;
}

}  // namespace platform
}  // namespace pzip
