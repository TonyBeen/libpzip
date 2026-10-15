#include "zip/zip_reader.h"

#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

namespace pzip {

ZipReader::ZipReader() : m_fileSize(0) {}

ZipReader::~ZipReader() { close(); }

pzip_status_t ZipReader::open(const std::string& archivePath) {
    close();
    m_path = archivePath;
    m_in.open(m_path.c_str(), std::ios::binary);
    if (!m_in.is_open()) {
        return PZIP_E_IO;
    }
    m_in.seekg(0, std::ios::end);
    const std::streampos pos = m_in.tellg();
    if (pos < 0) {
        close();
        return PZIP_E_IO;
    }
    m_fileSize = static_cast<uint64_t>(pos);
    m_in.seekg(0, std::ios::beg);
    return PZIP_OK;
}

void ZipReader::close() {
    if (m_in.is_open()) {
        m_in.close();
    }
    m_fileSize = 0;
    m_path.clear();
}

bool ZipReader::isOpen() const { return m_in.is_open(); }

pzip_status_t ZipReader::locateEocd(uint64_t* outEocdOffset, uint32_t* outCdOffset,
                                    uint32_t* outCdSize, uint16_t* outTotalEntries) {
    if (m_fileSize < 22) {
        return PZIP_E_NOT_SUPPORTED;
    }

    const uint64_t maxScan = 22U + 0xFFFFU;
    const uint64_t scanSize = (m_fileSize < maxScan) ? m_fileSize : maxScan;
    const uint64_t scanOffset = m_fileSize - scanSize;

    m_in.clear();
    m_in.seekg(static_cast<std::streamoff>(scanOffset), std::ios::beg);
    if (!m_in.good()) {
        return PZIP_E_IO;
    }

    std::vector<uint8_t> buffer(static_cast<size_t>(scanSize));
    m_in.read(reinterpret_cast<char*>(&buffer[0]), static_cast<std::streamsize>(scanSize));
    if (!m_in.good()) {
        return PZIP_E_IO;
    }

    const size_t bufLen = buffer.size();
    if (bufLen < 22) {
        return PZIP_E_NOT_SUPPORTED;
    }

    for (size_t i = bufLen - 22 + 1; i > 0; --i) {
        const size_t idx = i - 1;
        if (buffer[idx] == 0x50 && buffer[idx + 1] == 0x4b &&
            buffer[idx + 2] == 0x05 && buffer[idx + 3] == 0x06) {
            const uint16_t commentLen = static_cast<uint16_t>(
                buffer[idx + 20] | (static_cast<uint16_t>(buffer[idx + 21]) << 8));
            if (idx + 22 + commentLen == bufLen) {
                const uint16_t diskNum = static_cast<uint16_t>(
                    buffer[idx + 4] | (static_cast<uint16_t>(buffer[idx + 5]) << 8));
                const uint16_t cdDisk = static_cast<uint16_t>(
                    buffer[idx + 6] | (static_cast<uint16_t>(buffer[idx + 7]) << 8));
                if (diskNum != 0 || cdDisk != 0) {
                    return PZIP_E_NOT_SUPPORTED;
                }

                const uint16_t totalEntries = static_cast<uint16_t>(
                    buffer[idx + 10] | (static_cast<uint16_t>(buffer[idx + 11]) << 8));
                const uint32_t cdSize = static_cast<uint32_t>(
                    buffer[idx + 12] |
                    (static_cast<uint32_t>(buffer[idx + 13]) << 8) |
                    (static_cast<uint32_t>(buffer[idx + 14]) << 16) |
                    (static_cast<uint32_t>(buffer[idx + 15]) << 24));
                const uint32_t cdOffset = static_cast<uint32_t>(
                    buffer[idx + 16] |
                    (static_cast<uint32_t>(buffer[idx + 17]) << 8) |
                    (static_cast<uint32_t>(buffer[idx + 18]) << 16) |
                    (static_cast<uint32_t>(buffer[idx + 19]) << 24));

                const uint64_t eocdFileOffset = scanOffset + idx;
                if (static_cast<uint64_t>(cdOffset) + static_cast<uint64_t>(cdSize) > eocdFileOffset) {
                    return PZIP_E_NOT_SUPPORTED;
                }

                if (outEocdOffset != NULL) *outEocdOffset = eocdFileOffset;
                if (outCdOffset != NULL) *outCdOffset = cdOffset;
                if (outCdSize != NULL) *outCdSize = cdSize;
                if (outTotalEntries != NULL) *outTotalEntries = totalEntries;
                return PZIP_OK;
            }
        }
    }
    return PZIP_E_NOT_SUPPORTED;
}

pzip_status_t ZipReader::readCentralDirectory(std::vector<CentralRecord>* outRecords) {
    if (!m_in.is_open() || outRecords == NULL) {
        return PZIP_E_INVALID_ARG;
    }
    uint64_t eocdOffset = 0;
    uint32_t cdOffset = 0;
    uint32_t cdSize = 0;
    uint16_t totalEntries = 0;
    const pzip_status_t status = locateEocd(&eocdOffset, &cdOffset, &cdSize, &totalEntries);
    if (status != PZIP_OK) {
        return status;
    }

    outRecords->clear();
    outRecords->reserve(totalEntries);

    m_in.clear();
    m_in.seekg(static_cast<std::streamoff>(cdOffset), std::ios::beg);
    if (!m_in.good()) {
        return PZIP_E_IO;
    }

    for (size_t i = 0; i < totalEntries; ++i) {
        uint32_t sig = 0;
        m_in.read(reinterpret_cast<char*>(&sig), sizeof(sig));
        if (!m_in.good() || sig != 0x02014b50U) {
            return PZIP_E_NOT_SUPPORTED;
        }

        CentralRecord rec;
        uint16_t versionMadeBy = 0;
        uint16_t versionNeeded = 0;
        uint16_t flags = 0;
        uint16_t method = 0;
        uint16_t dosTime = 0;
        uint16_t dosDate = 0;
        uint32_t crc32 = 0;
        uint32_t compressedSize = 0;
        uint32_t uncompressedSize = 0;
        uint16_t nameLen = 0;
        uint16_t extraLen = 0;
        uint16_t commentLen = 0;
        uint16_t diskNumberStart = 0;
        uint16_t internalFileAttr = 0;
        uint32_t externalFileAttr = 0;
        uint32_t localOffset = 0;

        m_in.read(reinterpret_cast<char*>(&versionMadeBy), sizeof(versionMadeBy));
        m_in.read(reinterpret_cast<char*>(&versionNeeded), sizeof(versionNeeded));
        m_in.read(reinterpret_cast<char*>(&flags), sizeof(flags));
        m_in.read(reinterpret_cast<char*>(&method), sizeof(method));
        m_in.read(reinterpret_cast<char*>(&dosTime), sizeof(dosTime));
        m_in.read(reinterpret_cast<char*>(&dosDate), sizeof(dosDate));
        m_in.read(reinterpret_cast<char*>(&crc32), sizeof(crc32));
        m_in.read(reinterpret_cast<char*>(&compressedSize), sizeof(compressedSize));
        m_in.read(reinterpret_cast<char*>(&uncompressedSize), sizeof(uncompressedSize));
        m_in.read(reinterpret_cast<char*>(&nameLen), sizeof(nameLen));
        m_in.read(reinterpret_cast<char*>(&extraLen), sizeof(extraLen));
        m_in.read(reinterpret_cast<char*>(&commentLen), sizeof(commentLen));
        m_in.read(reinterpret_cast<char*>(&diskNumberStart), sizeof(diskNumberStart));
        m_in.read(reinterpret_cast<char*>(&internalFileAttr), sizeof(internalFileAttr));
        m_in.read(reinterpret_cast<char*>(&externalFileAttr), sizeof(externalFileAttr));
        m_in.read(reinterpret_cast<char*>(&localOffset), sizeof(localOffset));
        if (!m_in.good()) {
            return PZIP_E_IO;
        }

        if (compressedSize == 0xFFFFFFFFU || uncompressedSize == 0xFFFFFFFFU || localOffset == 0xFFFFFFFFU) {
            return PZIP_E_NOT_SUPPORTED;
        }

        std::string name(static_cast<size_t>(nameLen), '\0');
        if (nameLen > 0) {
            m_in.read(&name[0], static_cast<std::streamsize>(nameLen));
        }

        std::vector<uint8_t> extra(static_cast<size_t>(extraLen));
        if (extraLen > 0) {
            m_in.read(reinterpret_cast<char*>(&extra[0]), static_cast<std::streamsize>(extraLen));
        }

        std::string comment(static_cast<size_t>(commentLen), '\0');
        if (commentLen > 0) {
            m_in.read(&comment[0], static_cast<std::streamsize>(commentLen));
        }
        if (!m_in.good()) {
            return PZIP_E_IO;
        }

        rec.m_name = name;
        rec.m_extraField = extra;
        rec.m_comment = comment;
        rec.m_versionMadeBy = versionMadeBy;
        rec.m_versionNeeded = versionNeeded;
        rec.m_flags = flags;
        rec.m_method = method;
        rec.m_dosTime = dosTime;
        rec.m_dosDate = dosDate;
        rec.m_crc32 = crc32;
        rec.m_compressedSize = compressedSize;
        rec.m_uncompressedSize = uncompressedSize;
        rec.m_diskNumberStart = diskNumberStart;
        rec.m_internalFileAttr = internalFileAttr;
        rec.m_externalFileAttr = externalFileAttr;
        rec.m_localOffset = localOffset;

        outRecords->push_back(rec);
    }
    return PZIP_OK;
}

pzip_status_t ZipReader::readEntryPayload(const CentralRecord& record, std::vector<uint8_t>* outPayload) {
    if (!m_in.is_open() || outPayload == NULL) {
        return PZIP_E_INVALID_ARG;
    }
    m_in.clear();
    m_in.seekg(static_cast<std::streamoff>(record.m_localOffset), std::ios::beg);
    if (!m_in.good()) {
        return PZIP_E_IO;
    }

    uint32_t sig = 0;
    m_in.read(reinterpret_cast<char*>(&sig), sizeof(sig));
    if (!m_in.good() || sig != 0x04034b50U) {
        return PZIP_E_NOT_SUPPORTED;
    }

    m_in.seekg(22, std::ios::cur);
    uint16_t localNameLen = 0;
    uint16_t localExtraLen = 0;
    m_in.read(reinterpret_cast<char*>(&localNameLen), sizeof(localNameLen));
    m_in.read(reinterpret_cast<char*>(&localExtraLen), sizeof(localExtraLen));
    if (!m_in.good()) {
        return PZIP_E_IO;
    }

    m_in.seekg(static_cast<std::streamoff>(localNameLen + localExtraLen), std::ios::cur);
    if (!m_in.good()) {
        return PZIP_E_IO;
    }

    outPayload->resize(record.m_compressedSize);
    if (record.m_compressedSize > 0) {
        m_in.read(reinterpret_cast<char*>(&(*outPayload)[0]),
                  static_cast<std::streamsize>(record.m_compressedSize));
        if (!m_in.good()) {
            return PZIP_E_IO;
        }
    }
    return PZIP_OK;
}

pzip_status_t ZipReader::copyEntrySliceTo(const CentralRecord& record, const std::string& effectiveName,
                                          ZipWriter* writer, uint32_t* outNewLocalOffset) {
    if (!m_in.is_open() || writer == NULL || outNewLocalOffset == NULL) {
        return PZIP_E_INVALID_ARG;
    }
    m_in.clear();
    m_in.seekg(static_cast<std::streamoff>(record.m_localOffset), std::ios::beg);
    if (!m_in.good()) {
        return PZIP_E_IO;
    }

    uint32_t sig = 0;
    m_in.read(reinterpret_cast<char*>(&sig), sizeof(sig));
    if (!m_in.good() || sig != 0x04034b50U) {
        return PZIP_E_NOT_SUPPORTED;
    }

    m_in.seekg(22, std::ios::cur);
    uint16_t localNameLen = 0;
    uint16_t localExtraLen = 0;
    m_in.read(reinterpret_cast<char*>(&localNameLen), sizeof(localNameLen));
    m_in.read(reinterpret_cast<char*>(&localExtraLen), sizeof(localExtraLen));
    if (!m_in.good()) {
        return PZIP_E_IO;
    }

    const std::streamoff skipBytes = static_cast<std::streamoff>(localNameLen + localExtraLen);
    m_in.seekg(skipBytes, std::ios::cur);
    if (!m_in.good()) {
        return PZIP_E_IO;
    }

    const bool ok = writer->writeEntrySlice(record, effectiveName, m_in, outNewLocalOffset);
    return ok ? PZIP_OK : PZIP_E_IO;
}

pzip_status_t ZipReader::listArchive(const std::string& archivePath,
                                     pzip_list_callback_t callback,
                                     void* userData) {
    if (callback == NULL) {
        return PZIP_E_INVALID_ARG;
    }
    ZipReader reader;
    const pzip_status_t status = reader.open(archivePath);
    if (status != PZIP_OK) {
        return status;
    }
    std::vector<CentralRecord> records;
    const pzip_status_t cdStatus = reader.readCentralDirectory(&records);
    if (cdStatus != PZIP_OK) {
        return cdStatus;
    }

    for (size_t i = 0; i < records.size(); ++i) {
        const CentralRecord& rec = records[i];
        pzip_entry_info_t info;
        info.entry_name = rec.m_name.c_str();
        info.uncompressed_size = rec.m_uncompressedSize;
        info.compressed_size = rec.m_compressedSize;
        info.crc32 = rec.m_crc32;
        info.method = rec.m_method;
        info.flags = rec.m_flags;
        info.dos_time = rec.m_dosTime;
        info.dos_date = rec.m_dosDate;
        info.is_encrypted = ((rec.m_flags & 0x0001U) != 0U) ? 1 : 0;
        info.is_directory = ((!rec.m_name.empty() && rec.m_name[rec.m_name.size() - 1] == '/') ||
                             ((rec.m_externalFileAttr & 0x10U) != 0U) ||
                             (((rec.m_externalFileAttr >> 16U) & 0170000U) == 0040000U)) ? 1 : 0;

        const int32_t cbRet = callback(&info, userData);
        if (cbRet != 0) {
            break;
        }
    }
    return PZIP_OK;
}

}  // namespace pzip
