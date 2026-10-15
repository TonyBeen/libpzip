#include "zip/zip_writer.h"

#include <stdint.h>

#include <cstdlib>

#include <string>
#include <vector>

namespace pzip {

ZipWriter::ZipWriter(const std::string& archivePath)
    : m_out(archivePath.c_str(), std::ios::binary), m_failAfterBytes(-1), m_writtenBytes(0) {
#if defined(PZIP_ENABLE_TEST_HOOKS)
    const char* value = std::getenv("PZIP_TEST_FAIL_WRITE_AFTER_BYTES");
    if (value != NULL && value[0] != '\0') {
        char* end = NULL;
        const int64_t parsed = static_cast<int64_t>(std::strtoll(value, &end, 10));
        if (end != value && parsed >= 0) {
            m_failAfterBytes = parsed;
        }
    }
#endif
}

bool ZipWriter::isOpen() const { return m_out.is_open(); }

bool ZipWriter::writeEntry(const OutputFile& file, CentralRecord* rec) {
    if (!m_out.is_open() || rec == NULL) {
        return false;
    }

    if (file.m_entryName.size() > 0xFFFFU || file.m_extraField.size() > 0xFFFFU) {
        return false;
    }
    if (file.m_payload.size() > 0xFFFFFFFFULL) {
        return false;
    }

    const std::streampos pos = m_out.tellp();
    if (pos < 0 || static_cast<uint64_t>(pos) > 0xFFFFFFFFULL) {
        return false;
    }
    const uint32_t localOffset = static_cast<uint32_t>(pos);
    // General Purpose Bit 11 (0x0800U): Language encoding flag (EFS), indicating filename is UTF-8.
    const uint16_t flags = file.m_flags | 0x0800U;

    if (!writeU32(0x04034b50U) || !writeU16(20U) || !writeU16(flags) || !writeU16(file.m_method) ||
        !writeU16(static_cast<uint16_t>(file.m_dosTime)) ||
        !writeU16(static_cast<uint16_t>(file.m_dosDate)) || !writeU32(file.m_crc32) ||
        !writeU32(static_cast<uint32_t>(file.m_payload.size())) ||
        !writeU32(file.m_uncompressedSize) ||
        !writeU16(static_cast<uint16_t>(file.m_entryName.size())) ||
        !writeU16(static_cast<uint16_t>(file.m_extraField.size()))) {
        return false;
    }

    if (!writeRaw(file.m_entryName.data(), file.m_entryName.size())) {
        return false;
    }
    if (!file.m_extraField.empty() && !writeRaw(file.m_extraField.data(), file.m_extraField.size())) {
        return false;
    }
    if (!file.m_payload.empty() && !writeRaw(&file.m_payload[0], file.m_payload.size())) {
        return false;
    }

    rec->m_name = file.m_entryName;
    rec->m_extraField = file.m_extraField;
    rec->m_comment.clear();
    rec->m_versionMadeBy = 20U;
    rec->m_versionNeeded = 20U;
    rec->m_method = file.m_method;
    rec->m_flags = file.m_flags;
    rec->m_crc32 = file.m_crc32;
    rec->m_compressedSize = static_cast<uint32_t>(file.m_payload.size());
    rec->m_uncompressedSize = file.m_uncompressedSize;
    rec->m_localOffset = localOffset;
    rec->m_dosTime = file.m_dosTime;
    rec->m_dosDate = file.m_dosDate;
    rec->m_diskNumberStart = 0;
    rec->m_internalFileAttr = 0;
    rec->m_externalFileAttr = 0;
    return true;
}

bool ZipWriter::writeEntrySlice(const CentralRecord& record, const std::string& entryName,
                                std::istream& in, uint32_t* outLocalOffset) {
    if (!m_out.is_open() || outLocalOffset == NULL) {
        return false;
    }
    const std::string& name = entryName.empty() ? record.m_name : entryName;
    if (name.size() > 0xFFFFU || record.m_extraField.size() > 0xFFFFU) {
        return false;
    }

    const std::streampos pos = m_out.tellp();
    if (pos < 0 || static_cast<uint64_t>(pos) > 0xFFFFFFFFULL) {
        return false;
    }
    *outLocalOffset = static_cast<uint32_t>(pos);

    const uint16_t flags = (record.m_flags & ~0x0008U) | 0x0800U;
    const uint16_t versionNeeded = record.m_versionNeeded ? record.m_versionNeeded : 20U;

    if (!writeU32(0x04034b50U) || !writeU16(versionNeeded) || !writeU16(flags) ||
        !writeU16(record.m_method) || !writeU16(static_cast<uint16_t>(record.m_dosTime)) ||
        !writeU16(static_cast<uint16_t>(record.m_dosDate)) || !writeU32(record.m_crc32) ||
        !writeU32(record.m_compressedSize) || !writeU32(record.m_uncompressedSize) ||
        !writeU16(static_cast<uint16_t>(name.size())) ||
        !writeU16(static_cast<uint16_t>(record.m_extraField.size()))) {
        return false;
    }

    if (!writeRaw(name.data(), name.size())) {
        return false;
    }
    if (!record.m_extraField.empty() &&
        !writeRaw(record.m_extraField.data(), record.m_extraField.size())) {
        return false;
    }

    uint32_t remaining = record.m_compressedSize;
    uint8_t buffer[65536];
    while (remaining > 0) {
        const uint32_t chunk = (remaining > sizeof(buffer)) ? static_cast<uint32_t>(sizeof(buffer)) : remaining;
        in.read(reinterpret_cast<char*>(buffer), static_cast<std::streamsize>(chunk));
        if (!in.good()) {
            return false;
        }
        if (!writeRaw(buffer, chunk)) {
            return false;
        }
        remaining -= chunk;
    }
    return true;
}

bool ZipWriter::writeDirectory(const std::vector<CentralRecord>& records) {
    if (records.size() > 0xFFFFU) {
        return false;
    }

    const std::streampos pos = m_out.tellp();
    if (pos < 0 || static_cast<uint64_t>(pos) > 0xFFFFFFFFULL) {
        return false;
    }
    const uint32_t dirStart = static_cast<uint32_t>(pos);

    for (size_t i = 0; i < records.size(); ++i) {
        const CentralRecord& rec = records[i];
        if (rec.m_name.size() > 0xFFFFU || rec.m_extraField.size() > 0xFFFFU ||
            rec.m_comment.size() > 0xFFFFU) {
            return false;
        }
        const uint16_t flags = (rec.m_flags & ~0x0008U) | 0x0800U;
        const uint16_t versionMadeBy = rec.m_versionMadeBy ? rec.m_versionMadeBy : 20U;
        const uint16_t versionNeeded = rec.m_versionNeeded ? rec.m_versionNeeded : 20U;

        if (!writeU32(0x02014b50U) || !writeU16(versionMadeBy) || !writeU16(versionNeeded) ||
            !writeU16(flags) || !writeU16(rec.m_method) ||
            !writeU16(static_cast<uint16_t>(rec.m_dosTime)) ||
            !writeU16(static_cast<uint16_t>(rec.m_dosDate)) || !writeU32(rec.m_crc32) ||
            !writeU32(rec.m_compressedSize) || !writeU32(rec.m_uncompressedSize) ||
            !writeU16(static_cast<uint16_t>(rec.m_name.size())) ||
            !writeU16(static_cast<uint16_t>(rec.m_extraField.size())) ||
            !writeU16(static_cast<uint16_t>(rec.m_comment.size())) ||
            !writeU16(rec.m_diskNumberStart) || !writeU16(rec.m_internalFileAttr) ||
            !writeU32(rec.m_externalFileAttr) || !writeU32(rec.m_localOffset)) {
            return false;
        }
        if (!writeRaw(rec.m_name.data(), rec.m_name.size())) {
            return false;
        }
        if (!rec.m_extraField.empty() && !writeRaw(rec.m_extraField.data(), rec.m_extraField.size())) {
            return false;
        }
        if (!rec.m_comment.empty() && !writeRaw(rec.m_comment.data(), rec.m_comment.size())) {
            return false;
        }
    }

    const std::streampos endPos = m_out.tellp();
    if (endPos < 0 || static_cast<uint64_t>(endPos) > 0xFFFFFFFFULL) {
        return false;
    }
    const uint32_t dirEnd = static_cast<uint32_t>(endPos);
    const uint32_t dirSize = dirEnd - dirStart;
    const uint16_t count = static_cast<uint16_t>(records.size());

    return writeU32(0x06054b50U) && writeU16(0U) && writeU16(0U) && writeU16(count) &&
           writeU16(count) && writeU32(dirSize) && writeU32(dirStart) && writeU16(0U);
}

bool ZipWriter::writeRaw(const void* data, size_t size) {
    if (m_failAfterBytes >= 0) {
        const uint64_t threshold = static_cast<uint64_t>(m_failAfterBytes);
        if (m_writtenBytes + static_cast<uint64_t>(size) > threshold) {
            return false;
        }
    }
    m_out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!m_out.good()) {
        return false;
    }
    m_writtenBytes += static_cast<uint64_t>(size);
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
