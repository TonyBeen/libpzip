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
    std::vector<uint8_t> m_extraField;
    uint32_t m_crc32;
    uint16_t m_method;
    uint16_t m_flags;
    uint32_t m_dosTime;
    uint32_t m_dosDate;
    uint32_t m_uncompressedSize;

    OutputFile()
        : m_crc32(0),
          m_method(0),
          m_flags(0),
          m_dosTime(0),
          m_dosDate(0),
          m_uncompressedSize(0) {}
};

struct CentralRecord {
    std::string m_name;
    std::vector<uint8_t> m_extraField;
    std::string m_comment;
    uint16_t m_versionMadeBy;
    uint16_t m_versionNeeded;
    uint16_t m_method;
    uint16_t m_flags;
    uint32_t m_crc32;
    uint32_t m_compressedSize;
    uint32_t m_uncompressedSize;
    uint32_t m_localOffset;
    uint32_t m_dosTime;
    uint32_t m_dosDate;
    uint16_t m_diskNumberStart;
    uint16_t m_internalFileAttr;
    uint32_t m_externalFileAttr;

    CentralRecord()
        : m_versionMadeBy(20U),
          m_versionNeeded(20U),
          m_method(0),
          m_flags(0),
          m_crc32(0),
          m_compressedSize(0),
          m_uncompressedSize(0),
          m_localOffset(0),
          m_dosTime(0),
          m_dosDate(0),
          m_diskNumberStart(0),
          m_internalFileAttr(0),
          m_externalFileAttr(0) {}
};

class ZipWriter {
   public:
    explicit ZipWriter(const std::string& archivePath);

    bool isOpen() const;
    bool writeEntry(const OutputFile& file, CentralRecord* rec);
    bool writeEntrySlice(const CentralRecord& record, const std::string& entryName,
                         std::istream& in, uint32_t* outLocalOffset);
    bool writeDirectory(const std::vector<CentralRecord>& records);

   private:
    bool writeRaw(const void* data, size_t size);
    bool writeU16(uint16_t value);
    bool writeU32(uint32_t value);

    std::ofstream m_out;
    int64_t m_failAfterBytes;
    uint64_t m_writtenBytes;
};

}  // namespace pzip

#endif  // PZIP_ZIP_ZIP_WRITER_H_
