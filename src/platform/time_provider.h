#ifndef PZIP_PLATFORM_TIME_PROVIDER_H_
#define PZIP_PLATFORM_TIME_PROVIDER_H_

#include <stdint.h>

namespace pzip {
namespace platform {

class TimeProvider {
   public:
    void fillDosDateTime(uint32_t* outTime, uint32_t* outDate) const;
};

}  // namespace platform
}  // namespace pzip

#endif  // PZIP_PLATFORM_TIME_PROVIDER_H_
