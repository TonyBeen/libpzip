#include "io/file_reader.h"

#include <fstream>

namespace pzip {
namespace io {

pzip_status_t FileReader::readWholeFile(const std::string& path, std::vector<uint8_t>* out) const {
    if (out == NULL) {
        return PZIP_E_INVALID_ARG;
    }

    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in.is_open()) {
        return PZIP_E_IO;
    }

    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0) {
        return PZIP_E_IO;
    }

    in.seekg(0, std::ios::beg);
    out->resize(static_cast<size_t>(size));
    if (!out->empty()) {
        in.read(reinterpret_cast<char*>(&(*out)[0]), static_cast<std::streamsize>(out->size()));
        if (!in.good()) {
            return PZIP_E_IO;
        }
    }
    return PZIP_OK;
}

pzip_status_t FileReader::readFileInChunks(
    const std::string& path,
    size_t chunkBytes,
    const std::function<bool(size_t chunkIndex,
                             size_t totalChunks,
                             const uint8_t* data,
                             size_t size)>& onChunk) const {
    if (chunkBytes == 0 || !onChunk) {
        return PZIP_E_INVALID_ARG;
    }

    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in.is_open()) {
        return PZIP_E_IO;
    }

    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0) {
        return PZIP_E_IO;
    }
    in.seekg(0, std::ios::beg);

    const size_t fileSize = static_cast<size_t>(size);
    const size_t totalChunks = fileSize == 0 ? 1 : (fileSize + chunkBytes - 1) / chunkBytes;
    if (fileSize == 0) {
        return onChunk(0, totalChunks, NULL, 0) ? PZIP_OK : PZIP_E_CANCELED;
    }

    std::vector<uint8_t> buffer(chunkBytes);
    size_t remaining = fileSize;
    for (size_t chunkIndex = 0; chunkIndex < totalChunks; ++chunkIndex) {
        const size_t copyBytes = std::min(chunkBytes, remaining);
        in.read(reinterpret_cast<char*>(&buffer[0]), static_cast<std::streamsize>(copyBytes));
        if (in.gcount() != static_cast<std::streamsize>(copyBytes)) {
            return PZIP_E_IO;
        }

        if (!onChunk(chunkIndex, totalChunks, &buffer[0], copyBytes)) {
            return PZIP_E_CANCELED;
        }
        remaining -= copyBytes;
    }
    return PZIP_OK;
}

}  // namespace io
}  // namespace pzip
