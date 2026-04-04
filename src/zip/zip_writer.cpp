#include "zip/zip_writer.h"

#include <stdint.h>

#include <cstdlib>

#include <string>
#include <vector>

namespace pzip {

ZipWriter::ZipWriter(const std::string& archivePath)
    : m_out(archivePath.c_str(), std::ios::binary), m_failAfterBytes(-1), m_writtenBytes(0) {
    const char* value = std::getenv("PZIP_TEST_FAIL_WRITE_AFTER_BYTES");
    if (value != NULL && value[0] != '\0') {
        char* end = NULL;
        const long long parsed = std::strtoll(value, &end, 10);
        if (end != value && parsed >= 0) {
            m_failAfterBytes = parsed;
        }
    }
}

bool ZipWriter::isOpen() const { return m_out.is_open(); }

bool ZipWriter::writeEntry(const OutputFile& file, CentralRecord* rec) {
    if (!m_out.is_open() || rec == NULL) {
        return false;
    }

    const uint32_t localOffset = static_cast<uint32_t>(m_out.tellp());
    if (!writeU32(0x04034b50U) || !writeU16(20U) || !writeU16(0U) || !writeU16(file.m_method) ||
        !writeU16(static_cast<uint16_t>(file.m_dosTime)) ||
        !writeU16(static_cast<uint16_t>(file.m_dosDate)) || !writeU32(file.m_crc32) ||
        !writeU32(static_cast<uint32_t>(file.m_payload.size())) ||
        !writeU32(file.m_uncompressedSize) ||
        !writeU16(static_cast<uint16_t>(file.m_entryName.size())) || !writeU16(0U)) {
        return false;
    }

    if (!writeRaw(file.m_entryName.data(), file.m_entryName.size())) {
        return false;
    }
    if (!file.m_payload.empty() && !writeRaw(&file.m_payload[0], file.m_payload.size())) {
        return false;
    }

    rec->m_name = file.m_entryName;
    rec->m_method = file.m_method;
    rec->m_crc32 = file.m_crc32;
    rec->m_compressedSize = static_cast<uint32_t>(file.m_payload.size());
    rec->m_uncompressedSize = file.m_uncompressedSize;
    rec->m_localOffset = localOffset;
    rec->m_dosTime = file.m_dosTime;
    rec->m_dosDate = file.m_dosDate;
    return true;
}

bool ZipWriter::writeDirectory(const std::vector<CentralRecord>& records) {
    const uint32_t dirStart = static_cast<uint32_t>(m_out.tellp());

    for (size_t i = 0; i < records.size(); ++i) {
        const CentralRecord& rec = records[i];
        if (!writeU32(0x02014b50U) || !writeU16(20U) || !writeU16(20U) || !writeU16(0U) ||
            !writeU16(rec.m_method) || !writeU16(static_cast<uint16_t>(rec.m_dosTime)) ||
            !writeU16(static_cast<uint16_t>(rec.m_dosDate)) || !writeU32(rec.m_crc32) ||
            !writeU32(rec.m_compressedSize) || !writeU32(rec.m_uncompressedSize) ||
            !writeU16(static_cast<uint16_t>(rec.m_name.size())) || !writeU16(0U) || !writeU16(0U) ||
            !writeU16(0U) || !writeU16(0U) || !writeU32(0U) || !writeU32(rec.m_localOffset)) {
            return false;
        }
        if (!writeRaw(rec.m_name.data(), rec.m_name.size())) {
            return false;
        }
    }

    const uint32_t dirEnd = static_cast<uint32_t>(m_out.tellp());
    const uint32_t dirSize = dirEnd - dirStart;
    const uint16_t count = static_cast<uint16_t>(records.size());

    return writeU32(0x06054b50U) && writeU16(0U) && writeU16(0U) && writeU16(count) &&
           writeU16(count) && writeU32(dirSize) && writeU32(dirStart) && writeU16(0U);
}

bool ZipWriter::writeRaw(const void* data, size_t size) {
    if (m_failAfterBytes >= 0) {
        const unsigned long long threshold = static_cast<unsigned long long>(m_failAfterBytes);
        if (m_writtenBytes + static_cast<unsigned long long>(size) > threshold) {
            return false;
        }
    }
    m_out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!m_out.good()) {
        return false;
    }
    m_writtenBytes += static_cast<unsigned long long>(size);
    return true;
}

bool ZipWriter::writeU16(uint16_t value) {
    uint8_t bytes[2];
    bytes[0] = static_cast<uint8_t>(value & 0xFFU);
    bytes[1] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
    return writeRaw(bytes, sizeof(bytes));
}

bool ZipWriter::writeU32(uint32_t value) {
    uint8_t bytes[4];
    bytes[0] = static_cast<uint8_t>(value & 0xFFU);
    bytes[1] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
    bytes[2] = static_cast<uint8_t>((value >> 16U) & 0xFFU);
    bytes[3] = static_cast<uint8_t>((value >> 24U) & 0xFFU);
    return writeRaw(bytes, sizeof(bytes));
}

}  // namespace pzip
