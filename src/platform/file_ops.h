#ifndef PZIP_PLATFORM_FILE_OPS_H_
#define PZIP_PLATFORM_FILE_OPS_H_

#include <stdint.h>
#include <string>

namespace pzip {
namespace platform {

/**
 * @brief Atomically renames or replaces a file across all supported operating systems.
 * 
 * @param fromPathUtf8 Source file path in UTF-8.
 * @param toPathUtf8   Destination file path in UTF-8.
 * @return true if rename succeeded, false otherwise.
 */
bool AtomicRenameFile(const std::string& fromPathUtf8, const std::string& toPathUtf8);

/**
 * @brief Removes a file safely across platforms.
 * 
 * @param pathUtf8 File path in UTF-8.
 * @return true if deleted or did not exist, false if error.
 */
bool RemoveFile(const std::string& pathUtf8);

}  // namespace platform
}  // namespace pzip

#endif  // PZIP_PLATFORM_FILE_OPS_H_
