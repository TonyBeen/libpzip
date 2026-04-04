#ifndef PZIP_ZIP_ZIP_WRITER_H_
#define PZIP_ZIP_ZIP_WRITER_H_

#include <stdint.h>

#include <fstream>
#include <string>
#include <vector>

namespace pzip {

struct OutputFile {
    std::string m_entryName;
    std::vector<uint8_t> m_payload;
    uint32_t m_crc32;
    uint16_t m_method;
    uint32_t m_dosTime;
    uint32_t m_dosDate;
    uint32_t m_uncompressedSize;

    OutputFile() : m_crc32(0), m_method(0), m_dosTime(0), m_dosDate(0), m_uncompressedSize(0) {}
};

struct CentralRecord {
    std::string m_name;
    uint16_t m_method;
    uint32_t m_crc32;
    uint32_t m_compressedSize;
    uint32_t m_uncompressedSize;
    uint32_t m_localOffset;
    uint32_t m_dosTime;
    uint32_t m_dosDate;

    CentralRecord()
        : m_method(0),
          m_crc32(0),
          m_compressedSize(0),
          m_uncompressedSize(0),
          m_localOffset(0),
          m_dosTime(0),
          m_dosDate(0) {}
};

class ZipWriter {
   public:
    explicit ZipWriter(const std::string& archivePath);

    bool isOpen() const;
    bool writeEntry(const OutputFile& file, CentralRecord* rec);
    bool writeDirectory(const std::vector<CentralRecord>& records);

   private:
    bool writeRaw(const void* data, size_t size);
    bool writeU16(uint16_t value);
    bool writeU32(uint32_t value);

    std::ofstream m_out;
    long long m_failAfterBytes;
    unsigned long long m_writtenBytes;
};

}  // namespace pzip

#endif  // PZIP_ZIP_ZIP_WRITER_H_
