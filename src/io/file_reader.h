#ifndef PZIP_IO_FILE_READER_H_
#define PZIP_IO_FILE_READER_H_

#include <stdint.h>

#include <functional>
#include <string>
#include <vector>

#include "pzip.h"

namespace pzip {
namespace io {

class FileReader {
   public:
    pzip_status_t readWholeFile(const std::string& path, std::vector<uint8_t>* out) const;
    pzip_status_t readFileInChunks(
        const std::string& path,
        size_t chunkBytes,
        const std::function<bool(size_t chunkIndex,
                                 size_t totalChunks,
                                 const uint8_t* data,
                                 size_t size)>& onChunk) const;
};

}  // namespace io
}  // namespace pzip

#endif  // PZIP_IO_FILE_READER_H_
