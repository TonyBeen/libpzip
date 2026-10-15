#include "core/pzip_engine.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <limits>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "zlib.h"

#include "codec/lz4_codec.h"
#include "codec/zlib_codec.h"
#include "codec/zstd_codec.h"
#include "crypto/zip_crypto.h"
#include "platform/file_ops.h"
#include "zip/zip_reader.h"
#include "zip/zip_writer.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace pzip {

namespace fs = ghc::filesystem;

namespace {
static const char* kOkMessage = "OK";
static const uint32_t kDefaultChunkSizeKb = 256U;
static const uint32_t kDefaultMaxFileCount = 1000000U;
static const uint64_t kDefaultMaxInputBytes = 1ULL << 40;

#if defined(_WIN32)
bool IsValidUtf8(const std::string& str) {
    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(str.data());
    size_t i = 0;
    while (i < str.size()) {
        if (bytes[i] <= 0x7F) {
            i += 1;
        } else if ((bytes[i] & 0xE0) == 0xC0) {
            if (i + 1 >= str.size() || (bytes[i + 1] & 0xC0) != 0x80) return false;
            if (bytes[i] < 0xC2) return false;
            i += 2;
        } else if ((bytes[i] & 0xF0) == 0xE0) {
            if (i + 2 >= str.size() || (bytes[i + 1] & 0xC0) != 0x80 || (bytes[i + 2] & 0xC0) != 0x80) return false;
            if (bytes[i] == 0xE0 && bytes[i + 1] < 0xA0) return false;
            i += 3;
        } else if ((bytes[i] & 0xF8) == 0xF0) {
            if (i + 3 >= str.size() || (bytes[i + 1] & 0xC0) != 0x80 || (bytes[i + 2] & 0xC0) != 0x80 || (bytes[i + 3] & 0xC0) != 0x80) return false;
            if (bytes[i] == 0xF0 && bytes[i + 1] < 0x90) return false;
            i += 4;
        } else {
            return false;
        }
    }
    return true;
}

std::string EnsureUtf8(const std::string& str) {
    if (str.empty() || IsValidUtf8(str)) {
        return str;
    }
    int wideLen = ::MultiByteToWideChar(CP_ACP, 0, str.c_str(), static_cast<int>(str.size()), NULL, 0);
    if (wideLen <= 0) {
        return str;
    }
    std::wstring wide(static_cast<size_t>(wideLen), 0);
    ::MultiByteToWideChar(CP_ACP, 0, str.c_str(), static_cast<int>(str.size()), &wide[0], wideLen);

    int utf8Len = ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), wideLen, NULL, 0, NULL, NULL);
    if (utf8Len <= 0) {
        return str;
    }
    std::string utf8(static_cast<size_t>(utf8Len), 0);
    ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), wideLen, &utf8[0], utf8Len, NULL, NULL);
    return utf8;
}
#else
std::string EnsureUtf8(const std::string& str) {
    return str;
}
#endif

template <typename T>
class BlockingQueue {
   public:
    explicit BlockingQueue(size_t maxSize) : m_maxSize(maxSize), m_closed(false) {}

    bool push(const T& item) {
        std::unique_lock<std::mutex> lock(m_mu);
        m_notFull.wait(lock, [this]() { return m_closed || m_queue.size() < m_maxSize; });
        if (m_closed) {
            return false;
        }
        m_queue.push_back(item);
        m_notEmpty.notify_one();
        return true;
    }

    bool push(T&& item) {
        std::unique_lock<std::mutex> lock(m_mu);
        m_notFull.wait(lock, [this]() { return m_closed || m_queue.size() < m_maxSize; });
        if (m_closed) {
            return false;
        }
        m_queue.push_back(std::move(item));
        m_notEmpty.notify_one();
        return true;
    }

    bool pop(T* out) {
        if (out == NULL) {
            return false;
        }
        std::unique_lock<std::mutex> lock(m_mu);
        m_notEmpty.wait(lock, [this]() { return m_closed || !m_queue.empty(); });
        if (m_queue.empty()) {
            return false;
        }
        *out = std::move(m_queue.front());
        m_queue.pop_front();
        m_notFull.notify_one();
        return true;
    }

    void close() {
        std::lock_guard<std::mutex> lock(m_mu);
        m_closed = true;
        m_notEmpty.notify_all();
        m_notFull.notify_all();
    }

   private:
    size_t m_maxSize;
    bool m_closed;
    std::deque<T> m_queue;
    std::mutex m_mu;
    std::condition_variable m_notEmpty;
    std::condition_variable m_notFull;
};

class ChunkBufferPool {
   public:
    explicit ChunkBufferPool(size_t maxFreeBuffers)
        : m_maxFreeBuffers(maxFreeBuffers), m_totalFreeBuffers(0) {}

    std::vector<uint8_t> acquire(size_t bytes) {
        if (bytes == 0) {
            return std::vector<uint8_t>();
        }

        std::vector<uint8_t> buffer;
        const size_t bucket = bucketIndexFor(bytes);
        {
            std::lock_guard<std::mutex> lock(m_mu);
            if (bucket < kBucketCount && !m_freeBuckets[bucket].empty()) {
                buffer = std::move(m_freeBuckets[bucket].back());
                m_freeBuckets[bucket].pop_back();
                --m_totalFreeBuffers;
            }
        }

        if (buffer.capacity() < bytes) {
            if (bucket < kBucketCount) {
                buffer.reserve(kBucketCaps[bucket]);
            } else {
                buffer.reserve(bytes);
            }
        }
        buffer.resize(bytes);
        return buffer;
    }

    void release(std::vector<uint8_t>&& buffer) {
        if (buffer.capacity() == 0 || m_maxFreeBuffers == 0) {
            return;
        }

        const size_t bucket = bucketIndexFor(buffer.capacity());
        if (bucket >= kBucketCount) {
            return;
        }

        buffer.clear();
        std::lock_guard<std::mutex> lock(m_mu);
        if (m_totalFreeBuffers < m_maxFreeBuffers) {
            m_freeBuckets[bucket].push_back(std::move(buffer));
            ++m_totalFreeBuffers;
        }
    }

   private:
    static const size_t kBucketCount = 6;

    static const size_t kBucketCaps[kBucketCount];

    static size_t bucketIndexFor(size_t bytes) {
        for (size_t i = 0; i < kBucketCount; ++i) {
            if (bytes <= kBucketCaps[i]) {
                return i;
            }
        }
        return kBucketCount;
    }

    const size_t m_maxFreeBuffers;
    size_t m_totalFreeBuffers;
    std::vector<std::vector<uint8_t> > m_freeBuckets[kBucketCount];
    std::mutex m_mu;
};

const size_t ChunkBufferPool::kBucketCaps[ChunkBufferPool::kBucketCount] = {
    4U * 1024U,
    16U * 1024U,
    64U * 1024U,
    256U * 1024U,
    1024U * 1024U,
    4U * 1024U * 1024U,
};

struct FileChunkAccumulator {
    size_t m_totalChunks;
    size_t m_receivedChunks;
    size_t m_bufferedBytes;
    std::vector<std::vector<uint8_t> > m_chunks;

    FileChunkAccumulator() : m_totalChunks(0), m_receivedChunks(0), m_bufferedBytes(0) {}
};

struct PendingChunkShard {
    std::mutex m_mu;
    std::unordered_map<size_t, FileChunkAccumulator> m_files;
};
}  // namespace

PzipEngine::PzipEngine()
    : m_lastErrorCode(PZIP_OK),
      m_lastErrorMessage(kOkMessage),
      m_openMode(PZIP_MODE_CREATE),
      m_encryptionEnabled(false),
      m_canceled(false),
      m_running(false),
      m_progressCb(NULL),
      m_progressUserData(NULL),
      m_cryptoMode(PZIP_CRYPTO_NONE) {
    std::memset(&m_options, 0, sizeof(m_options));
    std::memset(&m_codec.m_vtable, 0, sizeof(m_codec.m_vtable));
    std::memset(&m_encryption.m_vtable, 0, sizeof(m_encryption.m_vtable));
    std::memset(&m_encryptionConfig, 0, sizeof(m_encryptionConfig));
    m_options.abi_version = PZIP_ABI_VERSION;
    m_options.thread_count = 0;
    m_options.chunk_size_kb = kDefaultChunkSizeKb;
    m_options.enable_solid_mode = 0;
    m_options.max_file_count = kDefaultMaxFileCount;
    m_options.max_total_input_bytes = kDefaultMaxInputBytes;
    m_encryptionConfig.abi_version = PZIP_ABI_VERSION;

    const pzip_codec_vtable_t defaultVtable = codec::CreateZstdCodecVtable();
    setCodec(&defaultVtable, NULL);
}

PzipEngine::~PzipEngine() {
    setCodec(NULL, NULL);
    setEncryption(NULL, NULL);
}

pzip_status_t PzipEngine::setProgressCallback(pzip_progress_callback_t callback, void* userData) {
    std::lock_guard<std::mutex> lock(m_mu);
    if (m_running) {
        return PZIP_E_BUSY;
    }
    m_progressCb = callback;
    m_progressUserData = userData;
    return PZIP_OK;
}

pzip_status_t PzipEngine::setPassword(const char* password) {
    std::lock_guard<std::mutex> lock(m_mu);
    if (m_running) {
        return PZIP_E_BUSY;
    }
    if (password == NULL || password[0] == '\0') {
        m_password.clear();
        m_cryptoMode = PZIP_CRYPTO_NONE;
    } else {
        m_password = password;
        if (m_cryptoMode == PZIP_CRYPTO_NONE) {
            m_cryptoMode = PZIP_CRYPTO_AES256;  // 默认标准加密 (WinZip AES-256)
        }
    }
    return PZIP_OK;
}

pzip_status_t PzipEngine::setCryptoMode(pzip_crypto_mode_t mode) {
    std::lock_guard<std::mutex> lock(m_mu);
    if (m_running) {
        return PZIP_E_BUSY;
    }
    m_cryptoMode = mode;
    return PZIP_OK;
}

pzip_status_t PzipEngine::setEncryption(const pzip_encryption_vtable_t* encryption,
                                        void* encryptionUser) {
    std::lock_guard<std::mutex> lock(m_mu);
    if (m_running) {
        return PZIP_E_BUSY;
    }

    if (m_encryption.m_vtable.destroy != NULL && m_encryption.m_instance != NULL) {
        m_encryption.m_vtable.destroy(m_encryption.m_instance);
        m_encryption.m_instance = NULL;
    }
    std::memset(&m_encryption.m_vtable, 0, sizeof(m_encryption.m_vtable));
    m_encryption.m_user = NULL;

    if (encryption == NULL) {
        return PZIP_OK;
    }
    if (encryption->abi_version != PZIP_ABI_VERSION || encryption->encrypt == NULL) {
        return PZIP_E_INVALID_ARG;
    }

    m_encryption.m_vtable = *encryption;
    m_encryption.m_user = encryptionUser;
    if (m_encryption.m_vtable.create != NULL) {
        m_encryption.m_instance = m_encryption.m_vtable.create(encryptionUser);
        if (m_encryption.m_instance == NULL) {
            return PZIP_E_CODEC;
        }
    }
    return PZIP_OK;
}

pzip_status_t PzipEngine::setEncryptionEnabled(uint32_t enabled) {
    std::lock_guard<std::mutex> lock(m_mu);
    if (m_running) {
        return PZIP_E_BUSY;
    }
    m_encryptionEnabled = (enabled != 0);
    return PZIP_OK;
}

pzip_status_t PzipEngine::setEncryptionConfig(const pzip_encryption_config_t* config) {
    if (config == NULL || config->abi_version != PZIP_ABI_VERSION) {
        return PZIP_E_INVALID_ARG;
    }

    std::lock_guard<std::mutex> lock(m_mu);
    if (m_running) {
        return PZIP_E_BUSY;
    }
    m_encryptionConfig = *config;
    return PZIP_OK;
}

pzip_status_t PzipEngine::getEncryptionConfig(pzip_encryption_config_t* outConfig) const {
    if (outConfig == NULL) {
        return PZIP_E_INVALID_ARG;
    }

    std::lock_guard<std::mutex> lock(m_mu);
    *outConfig = m_encryptionConfig;
    return PZIP_OK;
}

pzip_status_t PzipEngine::configure(const pzip_options_t* options) {
    if (options == NULL) {
        return PZIP_OK;
    }
    if (options->abi_version != PZIP_ABI_VERSION) {
        return PZIP_E_INVALID_ARG;
    }

    std::lock_guard<std::mutex> lock(m_mu);
    if (m_running) {
        return PZIP_E_BUSY;
    }

    m_options = *options;
    if (m_options.chunk_size_kb == 0) {
        m_options.chunk_size_kb = kDefaultChunkSizeKb;
    }
    if (m_options.max_file_count == 0) {
        m_options.max_file_count = kDefaultMaxFileCount;
    }
    if (m_options.max_total_input_bytes == 0) {
        m_options.max_total_input_bytes = kDefaultMaxInputBytes;
    }
    return PZIP_OK;
}

pzip_status_t PzipEngine::setCodec(const pzip_codec_vtable_t* codec, void* codecUser) {
    std::lock_guard<std::mutex> lock(m_mu);
    if (m_running) {
        return PZIP_E_BUSY;
    }

    if (m_codec.m_vtable.destroy != NULL && m_codec.m_instance != NULL) {
        m_codec.m_vtable.destroy(m_codec.m_instance);
        m_codec.m_instance = NULL;
    }
    std::memset(&m_codec.m_vtable, 0, sizeof(m_codec.m_vtable));
    m_codec.m_user = NULL;

    if (codec == NULL) {
        return PZIP_OK;
    }
    if (codec->abi_version != PZIP_ABI_VERSION || codec->compress == NULL) {
        return PZIP_E_INVALID_ARG;
    }

    m_codec.m_vtable = *codec;
    m_codec.m_user = codecUser;
    if (m_codec.m_vtable.create != NULL) {
        m_codec.m_instance = m_codec.m_vtable.create(codecUser);
        if (m_codec.m_instance == NULL) {
            return PZIP_E_CODEC;
        }
    }
    return PZIP_OK;
}

pzip_status_t PzipEngine::openArchive(const char* archivePath) {
    return openArchiveMode(archivePath, PZIP_MODE_CREATE);
}

pzip_status_t PzipEngine::openArchiveMode(const char* archivePath, pzip_open_mode_t mode) {
    if (archivePath == NULL || archivePath[0] == '\0') {
        return PZIP_E_INVALID_ARG;
    }
    if (mode < PZIP_MODE_CREATE || mode > PZIP_MODE_MODIFY) {
        return PZIP_E_INVALID_ARG;
    }

    std::lock_guard<std::mutex> lock(m_mu);
    if (m_running) {
        return PZIP_E_BUSY;
    }

    m_archivePath = archivePath;
    m_openMode = mode;
    m_files.clear();
    m_existingRecords.clear();
    m_pendingRemovals.clear();
    m_pendingRenames.clear();
    m_canceled.store(false);
    setError(PZIP_OK, kOkMessage);

    if (mode == PZIP_MODE_APPEND || mode == PZIP_MODE_MODIFY) {
        std::error_code ec;
        const fs::path p(EnsureUtf8(m_archivePath));
        if (fs::exists(p, ec) && !ec) {
            ZipReader reader;
            const pzip_status_t openStatus = reader.open(EnsureUtf8(m_archivePath));
            if (openStatus != PZIP_OK) {
                setError(openStatus, "failed to open existing archive");
                return openStatus;
            }
            const pzip_status_t cdStatus = reader.readCentralDirectory(&m_existingRecords);
            if (cdStatus != PZIP_OK) {
                setError(cdStatus, "failed to read central directory of existing archive");
                return cdStatus;
            }
        }
    }

    return PZIP_OK;
}

bool PzipEngine::isEntryRemoved(const std::string& entryName) const {
    for (size_t i = 0; i < m_pendingRemovals.size(); ++i) {
        const PendingRemoval& rem = m_pendingRemovals[i];
        if (rem.m_recursive == 0) {
            if (entryName == rem.m_pattern) {
                return true;
            }
        } else {
            const std::string& dir = rem.m_pattern;
            if (entryName == dir) {
                return true;
            }
            std::string dirSlash = dir;
            if (dirSlash.empty() || dirSlash[dirSlash.size() - 1] != '/') {
                dirSlash += '/';
            }
            if (entryName == dirSlash || entryName.compare(0, dirSlash.size(), dirSlash) == 0) {
                return true;
            }
        }
    }
    return false;
}

std::string PzipEngine::getRenamedName(const std::string& entryName) const {
    auto it = m_pendingRenames.find(entryName);
    if (it != m_pendingRenames.end()) {
        return it->second;
    }
    for (const auto& kv : m_pendingRenames) {
        std::string oldPrefix = kv.first;
        if (oldPrefix.empty()) {
            continue;
        }
        if (oldPrefix[oldPrefix.size() - 1] != '/') {
            oldPrefix += '/';
        }
        if (entryName.compare(0, oldPrefix.size(), oldPrefix) == 0) {
            std::string newPrefix = kv.second;
            if (!newPrefix.empty() && newPrefix[newPrefix.size() - 1] != '/') {
                newPrefix += '/';
            }
            return newPrefix + entryName.substr(oldPrefix.size());
        }
    }
    return entryName;
}

pzip_status_t PzipEngine::removeEntry(const char* entryName, int32_t recursive) {
    if (entryName == NULL || entryName[0] == '\0') {
        return PZIP_E_INVALID_ARG;
    }

    std::lock_guard<std::mutex> lock(m_mu);
    if (m_running) {
        return PZIP_E_BUSY;
    }

    const std::string norm = NormalizeEntryName(EnsureUtf8(entryName));
    if (norm.empty()) {
        return PZIP_E_INVALID_ARG;
    }

    PendingRemoval rem;
    rem.m_pattern = norm;
    rem.m_recursive = recursive;
    m_pendingRemovals.push_back(rem);

    for (auto it = m_files.begin(); it != m_files.end(); ) {
        bool match = false;
        if (recursive == 0) {
            match = (it->m_entryName == norm);
        } else {
            std::string dirSlash = norm;
            if (dirSlash.empty() || dirSlash[dirSlash.size() - 1] != '/') {
                dirSlash += '/';
            }
            match = (it->m_entryName == norm || it->m_entryName == dirSlash ||
                     it->m_entryName.compare(0, dirSlash.size(), dirSlash) == 0);
        }
        if (match) {
            it = m_files.erase(it);
        } else {
            ++it;
        }
    }

    return PZIP_OK;
}

pzip_status_t PzipEngine::renameEntry(const char* oldEntryName, const char* newEntryName) {
    if (oldEntryName == NULL || oldEntryName[0] == '\0' ||
        newEntryName == NULL || newEntryName[0] == '\0') {
        return PZIP_E_INVALID_ARG;
    }

    std::lock_guard<std::mutex> lock(m_mu);
    if (m_running) {
        return PZIP_E_BUSY;
    }

    const std::string oldNorm = NormalizeEntryName(EnsureUtf8(oldEntryName));
    const std::string newNorm = NormalizeEntryName(EnsureUtf8(newEntryName));
    if (oldNorm.empty() || newNorm.empty() || IsUnsafeEntry(newNorm)) {
        return PZIP_E_INVALID_ARG;
    }
    if (oldNorm == newNorm) {
        return PZIP_OK;
    }

    m_pendingRenames[oldNorm] = newNorm;

    for (size_t i = 0; i < m_files.size(); ++i) {
        if (m_files[i].m_entryName == oldNorm) {
            m_files[i].m_entryName = newNorm;
        }
    }

    return PZIP_OK;
}

pzip_status_t PzipEngine::hasEntry(const char* entryName, int32_t* outExists) const {
    if (entryName == NULL || outExists == NULL) {
        return PZIP_E_INVALID_ARG;
    }

    std::lock_guard<std::mutex> lock(m_mu);
    const std::string norm = NormalizeEntryName(EnsureUtf8(entryName));

    for (size_t i = 0; i < m_files.size(); ++i) {
        if (m_files[i].m_entryName == norm) {
            *outExists = 1;
            return PZIP_OK;
        }
    }

    for (size_t i = 0; i < m_existingRecords.size(); ++i) {
        if (isEntryRemoved(m_existingRecords[i].m_name)) {
            continue;
        }
        const std::string effective = getRenamedName(m_existingRecords[i].m_name);
        if (effective == norm) {
            *outExists = 1;
            return PZIP_OK;
        }
    }

    *outExists = 0;
    return PZIP_OK;
}

pzip_status_t PzipEngine::statEntry(const char* entryName, pzip_entry_info_t* outInfo) const {
    if (entryName == NULL || outInfo == NULL) {
        return PZIP_E_INVALID_ARG;
    }

    std::lock_guard<std::mutex> lock(m_mu);
    const std::string norm = NormalizeEntryName(EnsureUtf8(entryName));

    for (size_t i = 0; i < m_files.size(); ++i) {
        if (m_files[i].m_entryName == norm) {
            m_lastStatEntryName = m_files[i].m_entryName;
            outInfo->entry_name = m_lastStatEntryName.c_str();
            outInfo->uncompressed_size = m_files[i].m_fileSize;
            outInfo->compressed_size = 0;
            outInfo->crc32 = 0;
            outInfo->method = m_codec.m_vtable.zip_method;
            outInfo->flags = 0;
            outInfo->dos_time = 0;
            outInfo->dos_date = 0;
            outInfo->is_encrypted = (!m_password.empty() || m_encryptionEnabled) ? 1 : 0;
            outInfo->is_directory = (!norm.empty() && norm[norm.size() - 1] == '/') ? 1 : 0;
            return PZIP_OK;
        }
    }

    for (size_t i = 0; i < m_existingRecords.size(); ++i) {
        const CentralRecord& rec = m_existingRecords[i];
        if (isEntryRemoved(rec.m_name)) {
            continue;
        }
        const std::string effective = getRenamedName(rec.m_name);
        if (effective == norm) {
            m_lastStatEntryName = effective;
            outInfo->entry_name = m_lastStatEntryName.c_str();
            outInfo->uncompressed_size = rec.m_uncompressedSize;
            outInfo->compressed_size = rec.m_compressedSize;
            outInfo->crc32 = rec.m_crc32;
            outInfo->method = rec.m_method;
            outInfo->flags = rec.m_flags;
            outInfo->dos_time = rec.m_dosTime;
            outInfo->dos_date = rec.m_dosDate;
            outInfo->is_encrypted = ((rec.m_flags & 0x0001U) != 0U) ? 1 : 0;
            outInfo->is_directory = ((!rec.m_name.empty() && rec.m_name[rec.m_name.size() - 1] == '/') ||
                                     ((rec.m_externalFileAttr & 0x10U) != 0U) ||
                                     (((rec.m_externalFileAttr >> 16U) & 0170000U) == 0040000U)) ? 1 : 0;
            return PZIP_OK;
        }
    }

    return PZIP_E_NOT_FOUND;
}

pzip_status_t PzipEngine::getEntryCount(uint32_t* outCount) const {
    if (outCount == NULL) {
        return PZIP_E_INVALID_ARG;
    }

    std::lock_guard<std::mutex> lock(m_mu);
    uint32_t count = 0;
    for (size_t i = 0; i < m_existingRecords.size(); ++i) {
        if (isEntryRemoved(m_existingRecords[i].m_name)) {
            continue;
        }
        bool replaced = false;
        for (size_t j = 0; j < m_files.size(); ++j) {
            if (m_files[j].m_entryName == m_existingRecords[i].m_name) {
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            ++count;
        }
    }
    count += static_cast<uint32_t>(m_files.size());
    *outCount = count;
    return PZIP_OK;
}

pzip_status_t PzipEngine::listEntries(pzip_list_callback_t callback, void* userData) const {
    if (callback == NULL) {
        return PZIP_E_INVALID_ARG;
    }

    std::lock_guard<std::mutex> lock(m_mu);
    for (size_t i = 0; i < m_existingRecords.size(); ++i) {
        const CentralRecord& rec = m_existingRecords[i];
        if (isEntryRemoved(rec.m_name)) {
            continue;
        }
        bool replaced = false;
        for (size_t j = 0; j < m_files.size(); ++j) {
            if (m_files[j].m_entryName == rec.m_name) {
                replaced = true;
                break;
            }
        }
        if (replaced) {
            continue;
        }

        const std::string effective = getRenamedName(rec.m_name);
        pzip_entry_info_t info;
        info.entry_name = effective.c_str();
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
            return PZIP_OK;
        }
    }

    for (size_t i = 0; i < m_files.size(); ++i) {
        pzip_entry_info_t info;
        info.entry_name = m_files[i].m_entryName.c_str();
        info.uncompressed_size = m_files[i].m_fileSize;
        info.compressed_size = 0;
        info.crc32 = 0;
        info.method = m_codec.m_vtable.zip_method;
        info.flags = 0;
        info.dos_time = 0;
        info.dos_date = 0;
        info.is_encrypted = (!m_password.empty() || m_encryptionEnabled) ? 1 : 0;
        info.is_directory = (!m_files[i].m_entryName.empty() &&
                             m_files[i].m_entryName[m_files[i].m_entryName.size() - 1] == '/') ? 1 : 0;

        const int32_t cbRet = callback(&info, userData);
        if (cbRet != 0) {
            return PZIP_OK;
        }
    }

    return PZIP_OK;
}

pzip_status_t PzipEngine::clearPending() {
    std::lock_guard<std::mutex> lock(m_mu);
    if (m_running) {
        return PZIP_E_BUSY;
    }
    m_files.clear();
    m_pendingRemovals.clear();
    m_pendingRenames.clear();
    return PZIP_OK;
}

pzip_status_t PzipEngine::pushSingleFile(const fs::path& src, const std::string& prefix, uint64_t fileSize) {
    std::string entry = prefix.empty() ? src.filename().generic_u8string()
                                       : prefix + "/" + src.filename().generic_u8string();
    entry = NormalizeEntryName(entry);
    if (IsUnsafeEntry(entry) || entry.size() > 0xFFFFU) {
        return PZIP_E_INVALID_ARG;
    }
    if (m_files.size() >= m_options.max_file_count) {
        return PZIP_E_NOT_SUPPORTED;
    }

    PendingFile pending;
    pending.m_diskPath = src.string();
    pending.m_entryName = entry;
    pending.m_fileSize = fileSize;
    m_files.push_back(pending);
    return PZIP_OK;
}

pzip_status_t PzipEngine::addPath(const char* srcPath, const char* entryPrefix) {
    if (srcPath == NULL || srcPath[0] == '\0') {
        return PZIP_E_INVALID_ARG;
    }

    std::lock_guard<std::mutex> lock(m_mu);
    if (m_running) {
        return PZIP_E_BUSY;
    }

    std::error_code ec;
    const std::string utf8Src = EnsureUtf8(srcPath);
    const fs::path src(utf8Src);
    if (!fs::exists(src, ec) || ec) {
        setError(PZIP_E_NOT_FOUND, "path not found: " + std::string(srcPath));
        return PZIP_E_NOT_FOUND;
    }

    const std::string prefix = entryPrefix == NULL ? "" : NormalizeEntryName(EnsureUtf8(entryPrefix));

    if (fs::is_regular_file(src, ec) && !ec) {
        const uint64_t fSize = static_cast<uint64_t>(fs::file_size(src, ec));
        if (ec) {
            setError(PZIP_E_IO, "failed to get file size: " + ec.message());
            return PZIP_E_IO;
        }
        return pushSingleFile(src, prefix, fSize);
    }

    uint64_t totalBytes = 0;
    fs::recursive_directory_iterator it(src, ec);
    if (ec) {
        setError(PZIP_E_IO, "failed to open directory: " + ec.message());
        return PZIP_E_IO;
    }
    const fs::recursive_directory_iterator end;
    while (it != end) {
        const fs::path item = it->path();
        if (fs::is_regular_file(item, ec) && !ec) {
            const uint64_t fSize = static_cast<uint64_t>(fs::file_size(item, ec));
            if (ec) {
                setError(PZIP_E_IO, "failed to get file size: " + ec.message());
                return PZIP_E_IO;
            }

            totalBytes += fSize;
            if (totalBytes > m_options.max_total_input_bytes) {
                return PZIP_E_NOT_SUPPORTED;
            }

            std::string rel = MakeRelativeEntry(item, src);
            std::string entry = prefix.empty() ? rel : prefix + "/" + rel;
            entry = NormalizeEntryName(entry);
            if (IsUnsafeEntry(entry) || entry.size() > 0xFFFFU) {
                return PZIP_E_INVALID_ARG;
            }
            if (m_files.size() >= m_options.max_file_count) {
                return PZIP_E_NOT_SUPPORTED;
            }

            PendingFile pending;
            pending.m_diskPath = item.string();
            pending.m_entryName = entry;
            pending.m_fileSize = fSize;
            m_files.push_back(pending);
        }

        it.increment(ec);
        if (ec) {
            setError(PZIP_E_IO, "failed to iterate directory: " + ec.message());
            return PZIP_E_IO;
        }
    }

    return PZIP_OK;
}

pzip_status_t PzipEngine::validateReadyForRun() const {
    if (m_archivePath.empty()) {
        return PZIP_E_INVALID_ARG;
    }
    if (m_openMode == PZIP_MODE_CREATE) {
        if (m_files.empty()) {
            return PZIP_E_INVALID_ARG;
        }
    } else {
        if (m_files.empty() && m_pendingRemovals.empty() && m_pendingRenames.empty()) {
            return PZIP_E_INVALID_ARG;
        }
    }
    return PZIP_OK;
}

pzip_status_t PzipEngine::run() {
    {
        std::lock_guard<std::mutex> lock(m_mu);
        if (m_running) {
            return PZIP_E_BUSY;
        }
        const pzip_status_t status = validateReadyForRun();
        if (status != PZIP_OK) {
            setError(status, "archive path or files not ready");
            return status;
        }
        if (m_encryptionEnabled && m_encryption.m_vtable.encrypt == NULL) {
            setError(PZIP_E_INVALID_ARG, "encryption enabled but encrypt callback is not set");
            return PZIP_E_INVALID_ARG;
        }
        m_running = true;
        m_canceled.store(false);
    }

    const auto failAndExit = [this](pzip_status_t status, const char* msg) {
        setError(status, msg);
        std::lock_guard<std::mutex> lock(m_mu);
        m_running = false;
        return status;
    };

    const bool isRepack = (m_openMode != PZIP_MODE_CREATE && !m_existingRecords.empty());
    std::string tempPath;
    const std::string targetArchivePath = EnsureUtf8(m_archivePath);
    if (isRepack) {
        const uint64_t now = static_cast<uint64_t>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count());
        static std::atomic<uint64_t> s_repackCounter(0);
        tempPath = targetArchivePath + ".pzip_tmp_" +
                   std::to_string(now) + "_" +
                   std::to_string(++s_repackCounter);
    }
    const std::string outPath = isRepack ? tempPath : targetArchivePath;

    if (isRepack && m_files.empty()) {
        ZipWriter writer(tempPath);
        if (!writer.isOpen()) {
            return failAndExit(PZIP_E_IO, "failed to create temporary archive");
        }

        ZipReader reader;
        if (reader.open(targetArchivePath) != PZIP_OK) {
            platform::RemoveFile(tempPath);
            return failAndExit(PZIP_E_IO, "failed to open existing archive");
        }

        std::vector<CentralRecord> records;
        for (size_t i = 0; i < m_existingRecords.size(); ++i) {
            if (m_canceled.load()) {
                reader.close();
                platform::RemoveFile(tempPath);
                return failAndExit(PZIP_E_CANCELED, "canceled");
            }
            const CentralRecord& oldRec = m_existingRecords[i];
            if (isEntryRemoved(oldRec.m_name)) {
                continue;
            }
            const std::string effectiveName = getRenamedName(oldRec.m_name);
            CentralRecord newRec = oldRec;
            uint32_t newLocalOffset = 0;
            const pzip_status_t copyStatus =
                reader.copyEntrySliceTo(oldRec, effectiveName, &writer, &newLocalOffset);
            if (copyStatus != PZIP_OK) {
                reader.close();
                platform::RemoveFile(tempPath);
                return failAndExit(copyStatus, "failed to stream-copy entry slice");
            }
            newRec.m_name = effectiveName;
            newRec.m_localOffset = newLocalOffset;
            records.push_back(newRec);
        }
        reader.close();

        if (!writer.writeDirectory(records)) {
            platform::RemoveFile(tempPath);
            return failAndExit(PZIP_E_IO, "failed to write central directory");
        }
        if (!platform::AtomicRenameFile(tempPath, targetArchivePath)) {
            platform::RemoveFile(tempPath);
            return failAndExit(PZIP_E_IO, "failed to atomically rename archive");
        }
        m_existingRecords = records;
        m_pendingRemovals.clear();
        m_pendingRenames.clear();
        setError(PZIP_OK, kOkMessage);
        {
            std::lock_guard<std::mutex> lock(m_mu);
            m_running = false;
        }
        return PZIP_OK;
    }

    const uint32_t cpuThreads = (std::max)(1U, std::thread::hardware_concurrency());
    const uint32_t defaultThreads = (std::min)(16U, cpuThreads);
    const uint32_t threadCount = m_options.thread_count == 0 ? defaultThreads : m_options.thread_count;
    const size_t chunkBytes =
        (std::max<size_t>)(1, static_cast<size_t>(m_options.chunk_size_kb) * static_cast<size_t>(1024));
    const size_t queueCapacity = (std::max<size_t>)(4, static_cast<size_t>(threadCount) * 2);
    const size_t defaultPendingBytesLimit =
        (std::max<size_t>)(chunkBytes * static_cast<size_t>(threadCount) * 4, chunkBytes * 8);
    const size_t pendingBytesLimit = (std::max<size_t>)(chunkBytes, (std::min<size_t>)(
                                                                  defaultPendingBytesLimit,
                                                                  static_cast<size_t>(m_options.max_total_input_bytes)));
    const size_t pendingShardCount = (std::max<size_t>)(1, (std::min<size_t>)(static_cast<size_t>(threadCount), 8));
    const size_t maxFreeChunkBuffers = queueCapacity * 2;

    BlockingQueue<ChunkTask> chunkQueue(queueCapacity);
    BlockingQueue<WriteTask> writeQueue(queueCapacity);
    ChunkBufferPool chunkBufferPool(maxFreeChunkBuffers);
    std::atomic<int32_t> firstError(PZIP_OK);
    std::atomic<uint32_t> aliveWorkers(threadCount);
    std::mutex pendingBytesMu;
    std::condition_variable pendingChunksCv;
    size_t pendingBufferedBytes = 0;
    std::vector<PendingChunkShard> pendingShards(pendingShardCount);

    auto markFailed = [this, &firstError, &pendingChunksCv](int32_t code) {
        int32_t expected = PZIP_OK;
        firstError.compare_exchange_strong(expected, code);
        m_canceled.store(true);
        pendingChunksCv.notify_all();
    };

    std::thread reader([this, &chunkQueue, &markFailed, &chunkBufferPool, chunkBytes]() {
        for (size_t i = 0; i < m_files.size() && !m_canceled.load(); ++i) {
            FileTask fileTask;
            fileTask.m_fileIndex = i;

            const pzip_status_t status = m_fileReader.readFileInChunks(
                m_files[fileTask.m_fileIndex].m_diskPath,
                chunkBytes,
                [this, &chunkQueue, &fileTask, &chunkBufferPool](size_t chunkIndex,
                                                                 size_t totalChunks,
                                                                 const uint8_t* data,
                                                                 size_t size) -> bool {
                    if (m_canceled.load()) {
                        return false;
                    }

                    ChunkTask chunkTask;
                    chunkTask.m_fileIndex = fileTask.m_fileIndex;
                    chunkTask.m_chunkIndex = chunkIndex;
                    chunkTask.m_totalChunks = totalChunks;
                    if (size > 0) {
                        std::vector<uint8_t> pooled = chunkBufferPool.acquire(size);
                        if (data != NULL) {
                            std::memcpy(&pooled[0], data, size);
                        }
                        chunkTask.m_rawData.swap(pooled);
                    }
                    return chunkQueue.push(std::move(chunkTask));
                });
            if (status == PZIP_E_CANCELED) {
                if (m_canceled.load()) {
                    break;
                }
                markFailed(PZIP_E_CANCELED);
                break;
            }
            if (status != PZIP_OK) {
                markFailed(status);
                break;
            }
        }
        chunkQueue.close();
    });

    std::vector<std::thread> workers;
    workers.reserve(threadCount);
    for (uint32_t i = 0; i < threadCount; ++i) {
        workers.push_back(std::thread(
            [this,
             &chunkQueue,
             &writeQueue,
             &markFailed,
             &aliveWorkers,
             &pendingBytesMu,
             &pendingChunksCv,
             &pendingBufferedBytes,
             pendingBytesLimit,
             pendingShardCount,
             &chunkBufferPool,
             &pendingShards]() {
            void* localCodecCtx = NULL;
            if (m_codec.m_vtable.create != NULL) {
                localCodecCtx = m_codec.m_vtable.create(m_codec.m_user);
                if (localCodecCtx == NULL) {
                    markFailed(PZIP_E_CODEC);
                    if (aliveWorkers.fetch_sub(1) == 1) {
                        writeQueue.close();
                    }
                    return;
                }
            }

            ChunkTask chunkTask;
            std::vector<uint8_t> reusableMergeBuffer;
            while (chunkQueue.pop(&chunkTask)) {
                if (m_canceled.load()) {
                    break;
                }

                ChunkTask mergedChunk;
                bool fileReady = false;
                bool fileInProgress = false;
                bool reservedBytes = false;
                std::vector<std::vector<uint8_t> > readyChunks;
                size_t bufferedBytesToRelease = 0;
                const size_t chunkSize = chunkTask.m_rawData.size();
                PendingChunkShard& shard = pendingShards[chunkTask.m_fileIndex % pendingShardCount];
                {
                    std::lock_guard<std::mutex> lock(shard.m_mu);
                    fileInProgress = shard.m_files.find(chunkTask.m_fileIndex) != shard.m_files.end();
                }

                {
                    std::unique_lock<std::mutex> lock(pendingBytesMu);
                    pendingChunksCv.wait(
                        lock,
                        [this, pendingBytesLimit, chunkSize, fileInProgress, &pendingBufferedBytes]() {
                            return m_canceled.load() || fileInProgress ||
                                   (pendingBufferedBytes + chunkSize <= pendingBytesLimit);
                        });
                    if (m_canceled.load()) {
                        break;
                    }
                    pendingBufferedBytes += chunkSize;
                    reservedBytes = true;
                }

                {
                    std::lock_guard<std::mutex> lock(shard.m_mu);
                    FileChunkAccumulator& acc = shard.m_files[chunkTask.m_fileIndex];
                    if (acc.m_totalChunks == 0) {
                        acc.m_totalChunks = chunkTask.m_totalChunks;
                        acc.m_chunks.resize(acc.m_totalChunks);
                    }
                    if (chunkTask.m_chunkIndex >= acc.m_totalChunks) {
                        bufferedBytesToRelease = chunkSize;
                    } else if (acc.m_chunks[chunkTask.m_chunkIndex].empty()) {
                        acc.m_bufferedBytes += chunkSize;
                        acc.m_chunks[chunkTask.m_chunkIndex] = std::move(chunkTask.m_rawData);
                        ++acc.m_receivedChunks;
                    } else {
                        bufferedBytesToRelease = chunkSize;
                    }

                    if (acc.m_receivedChunks == acc.m_totalChunks) {
                        mergedChunk.m_fileIndex = chunkTask.m_fileIndex;
                        mergedChunk.m_chunkIndex = 0;
                        mergedChunk.m_totalChunks = 1;

                        readyChunks.swap(acc.m_chunks);
                        bufferedBytesToRelease += acc.m_bufferedBytes;
                        shard.m_files.erase(chunkTask.m_fileIndex);
                        fileReady = true;
                    }
                }

                if (bufferedBytesToRelease > 0 && reservedBytes) {
                    std::lock_guard<std::mutex> lock(pendingBytesMu);
                    if (pendingBufferedBytes >= bufferedBytesToRelease) {
                        pendingBufferedBytes -= bufferedBytesToRelease;
                    } else {
                        pendingBufferedBytes = 0;
                    }
                    pendingChunksCv.notify_all();
                }

                if (bufferedBytesToRelease > 0 && !fileReady) {
                    if (!chunkTask.m_rawData.empty()) {
                        chunkBufferPool.release(std::move(chunkTask.m_rawData));
                    }
                    markFailed(PZIP_E_INTERNAL);
                    break;
                }

                if (!fileReady) {
                    continue;
                }

                size_t totalBytes = 0;
                for (size_t c = 0; c < readyChunks.size(); ++c) {
                    totalBytes += readyChunks[c].size();
                }
                reusableMergeBuffer.clear();
                if (reusableMergeBuffer.capacity() < totalBytes) {
                    reusableMergeBuffer.reserve(totalBytes);
                }
                for (size_t c = 0; c < readyChunks.size(); ++c) {
                    if (!readyChunks[c].empty()) {
                        reusableMergeBuffer.insert(reusableMergeBuffer.end(), readyChunks[c].begin(),
                                                   readyChunks[c].end());
                    }
                    chunkBufferPool.release(std::move(readyChunks[c]));
                }
                mergedChunk.m_rawData.swap(reusableMergeBuffer);

                WriteTask writeTask;
                const pzip_status_t status = compressChunkTask(&mergedChunk, &writeTask, localCodecCtx);
                if (status != PZIP_OK) {
                    markFailed(status);
                    break;
                }
                reusableMergeBuffer.swap(mergedChunk.m_rawData);
                if (!writeQueue.push(std::move(writeTask))) {
                    break;
                }
            }
            if (m_codec.m_vtable.destroy != NULL && localCodecCtx != NULL) {
                m_codec.m_vtable.destroy(localCodecCtx);
                localCodecCtx = NULL;
            }
            if (aliveWorkers.fetch_sub(1) == 1) {
                writeQueue.close();
            }
        }));
    }

    std::thread writerThread([this, &writeQueue, &markFailed, &firstError, isRepack, outPath, targetArchivePath]() {
        std::vector<CentralRecord> records;
        {
            ZipWriter writer(outPath);
            if (!writer.isOpen()) {
                markFailed(PZIP_E_IO);
                writeQueue.close();
                return;
            }

            if (isRepack) {
                ZipReader reader;
                if (reader.open(targetArchivePath) != PZIP_OK) {
                    markFailed(PZIP_E_IO);
                    writeQueue.close();
                    return;
                }
                for (size_t i = 0; i < m_existingRecords.size(); ++i) {
                    if (m_canceled.load()) {
                        reader.close();
                        writeQueue.close();
                        return;
                    }
                    const CentralRecord& oldRec = m_existingRecords[i];
                    if (isEntryRemoved(oldRec.m_name)) {
                        continue;
                    }
                    bool replaced = false;
                    for (size_t j = 0; j < m_files.size(); ++j) {
                        if (m_files[j].m_entryName == oldRec.m_name) {
                            replaced = true;
                            break;
                        }
                    }
                    if (replaced) {
                        continue;
                    }

                    const std::string effectiveName = getRenamedName(oldRec.m_name);
                    CentralRecord newRec = oldRec;
                    uint32_t newLocalOffset = 0;
                    const pzip_status_t copyStatus =
                        reader.copyEntrySliceTo(oldRec, effectiveName, &writer, &newLocalOffset);
                    if (copyStatus != PZIP_OK) {
                        reader.close();
                        markFailed(copyStatus);
                        writeQueue.close();
                        return;
                    }
                    newRec.m_name = effectiveName;
                    newRec.m_localOffset = newLocalOffset;
                    records.push_back(newRec);
                }
                reader.close();
            }

            uint64_t totalBytes = 0;
            for (size_t i = 0; i < m_files.size(); ++i) {
                totalBytes += m_files[i].m_fileSize;
            }
            uint64_t processedBytes = 0;

            records.reserve(records.size() + m_files.size());
            std::vector<OutputFile> pendingOutputs(m_files.size());
            std::vector<uint8_t> pendingReady(m_files.size(), 0);
            size_t expectedIndex = 0;
            size_t written = 0;

            WriteTask writeTask;
            while (writeQueue.pop(&writeTask)) {
                if (writeTask.m_fileIndex >= pendingOutputs.size() || pendingReady[writeTask.m_fileIndex] != 0) {
                    markFailed(PZIP_E_INTERNAL);
                    writeQueue.close();
                    return;
                }
                pendingOutputs[writeTask.m_fileIndex] = std::move(writeTask.m_output);
                pendingReady[writeTask.m_fileIndex] = 1;

                while (expectedIndex < pendingOutputs.size() && pendingReady[expectedIndex] != 0) {
                    if (m_encryptionEnabled) {
                        OutputFile* outFile = &pendingOutputs[expectedIndex];
                        size_t dstCap = outFile->m_payload.size();
                        if (m_encryption.m_vtable.bound != NULL) {
                            dstCap = m_encryption.m_vtable.bound(m_encryption.m_instance,
                                                                 outFile->m_payload.size());
                        }
                        if (dstCap < outFile->m_payload.size()) {
                            dstCap = outFile->m_payload.size();
                        }

                        std::vector<uint8_t> encrypted(dstCap);
                        size_t dstSize = 0;
                        const pzip_status_t encStatus =
                            m_encryption.m_vtable.encrypt(m_encryption.m_instance,
                                                          outFile->m_payload.empty()
                                                              ? NULL
                                                              : &outFile->m_payload[0],
                                                          outFile->m_payload.size(),
                                                          encrypted.empty() ? NULL : &encrypted[0],
                                                          encrypted.size(),
                                                          &dstSize);
                        if (encStatus != PZIP_OK) {
                            markFailed(static_cast<int32_t>(encStatus));
                            writeQueue.close();
                            return;
                        }
                        if (dstSize > encrypted.size()) {
                            markFailed(PZIP_E_INTERNAL);
                            writeQueue.close();
                            return;
                        }
                        encrypted.resize(dstSize);
                        outFile->m_payload.swap(encrypted);
                    }

                    if (!m_password.empty() && m_cryptoMode != PZIP_CRYPTO_NONE) {
                        OutputFile* outFile = &pendingOutputs[expectedIndex];
                        if (m_cryptoMode == PZIP_CRYPTO_AES256 || m_cryptoMode == PZIP_CRYPTO_AES128) {
                            const int32_t strength = (m_cryptoMode == PZIP_CRYPTO_AES256) ? 3 : 1;
                            const uint16_t actualMethod = outFile->m_method;
                            std::vector<uint8_t> encrypted;
                            if (!crypto::WinZipAesEncrypt(m_password, strength,
                                                          outFile->m_payload.empty() ? NULL : &outFile->m_payload[0],
                                                          outFile->m_payload.size(), &encrypted)) {
                                markFailed(PZIP_E_CODEC);
                                writeQueue.close();
                                return;
                            }
                            outFile->m_payload.swap(encrypted);
                            outFile->m_flags |= 0x0001U;
                            outFile->m_method = 99;
                            outFile->m_crc32 = 0;  // WinZip AE-2: CRC is 0
                            outFile->m_extraField.resize(11);
                            outFile->m_extraField[0] = 0x01;
                            outFile->m_extraField[1] = 0x99;
                            outFile->m_extraField[2] = 0x07;
                            outFile->m_extraField[3] = 0x00;
                            outFile->m_extraField[4] = 0x02;  // AE-2 vendor version
                            outFile->m_extraField[5] = 0x00;
                            outFile->m_extraField[6] = 0x41;  // 'A'
                            outFile->m_extraField[7] = 0x45;  // 'E'
                            outFile->m_extraField[8] = static_cast<uint8_t>(strength);
                            outFile->m_extraField[9] = static_cast<uint8_t>(actualMethod & 0xFF);
                            outFile->m_extraField[10] = static_cast<uint8_t>((actualMethod >> 8) & 0xFF);
                        } else if (m_cryptoMode == PZIP_CRYPTO_ZIPCRYPTO) {
                            std::vector<uint8_t> encrypted;
                            if (!crypto::ZipCryptoEncrypt(m_password, outFile->m_crc32,
                                                          outFile->m_payload.empty() ? NULL : &outFile->m_payload[0],
                                                          outFile->m_payload.size(), &encrypted)) {
                                markFailed(PZIP_E_CODEC);
                                writeQueue.close();
                                return;
                            }
                            outFile->m_payload.swap(encrypted);
                            outFile->m_flags |= 0x0001U;
                        }
                    }

                    CentralRecord rec;
                    if (!writer.writeEntry(pendingOutputs[expectedIndex], &rec)) {
                        markFailed(PZIP_E_IO);
                        writeQueue.close();
                        return;
                    }
                    records.push_back(rec);
                    if (expectedIndex < m_files.size()) {
                        processedBytes += m_files[expectedIndex].m_fileSize;
                    }
                    if (m_progressCb != NULL) {
                        m_progressCb(processedBytes, totalBytes, m_progressUserData);
                    }
                    pendingOutputs[expectedIndex] = OutputFile();
                    pendingReady[expectedIndex] = 0;
                    ++expectedIndex;
                    ++written;
                }
            }

            if (!m_canceled.load() && written != m_files.size()) {
                markFailed(PZIP_E_INTERNAL);
                return;
            }
            if (!m_canceled.load() && !writer.writeDirectory(records)) {
                markFailed(PZIP_E_IO);
            } else if (!m_canceled.load() && m_progressCb != NULL && processedBytes < totalBytes) {
                m_progressCb(totalBytes, totalBytes, m_progressUserData);
            }
        }

        if (!m_canceled.load() && firstError.load() == PZIP_OK && isRepack) {
            if (!platform::AtomicRenameFile(outPath, targetArchivePath)) {
                markFailed(PZIP_E_IO);
            } else {
                m_existingRecords = records;
            }
        }
    });

    reader.join();
    for (size_t i = 0; i < workers.size(); ++i) {
        workers[i].join();
    }
    writerThread.join();

    if (m_canceled.load() || firstError.load() != PZIP_OK) {
        if (isRepack) {
            platform::RemoveFile(tempPath);
        }
        const int32_t err = firstError.load() == PZIP_OK ? PZIP_E_CANCELED : firstError.load();
        setError(err, "compression canceled or failed");
        std::lock_guard<std::mutex> lock(m_mu);
        m_running = false;
        return static_cast<pzip_status_t>(err);
    }

    if (isRepack) {
        m_pendingRemovals.clear();
        m_pendingRenames.clear();
    }
    m_files.clear();

    setError(PZIP_OK, kOkMessage);
    {
        std::lock_guard<std::mutex> lock(m_mu);
        m_running = false;
    }
    return PZIP_OK;
}

pzip_status_t PzipEngine::extractArchive(const char* archivePath, const char* outputDir) {
    if (archivePath == NULL || archivePath[0] == '\0' || outputDir == NULL || outputDir[0] == '\0') {
        return PZIP_E_INVALID_ARG;
    }

    {
        std::lock_guard<std::mutex> lock(m_mu);
        if (m_running) {
            return PZIP_E_BUSY;
        }
        m_running = true;
        m_canceled.store(false);
    }

    const auto failAndExit = [this](pzip_status_t status, const char* msg) {
        setError(status, msg);
        std::lock_guard<std::mutex> lock(m_mu);
        m_running = false;
        return status;
    };

    std::ifstream in(EnsureUtf8(archivePath).c_str(), std::ios::binary);
    if (!in.is_open()) {
        return failAndExit(PZIP_E_IO, "failed to open archive for extract");
    }

    in.seekg(0, std::ios::end);
    const uint64_t totalArchiveBytes = static_cast<uint64_t>(in.tellg());
    in.seekg(0, std::ios::beg);

    const fs::path outputRoot(EnsureUtf8(outputDir));
    std::error_code ec;
    fs::create_directories(outputRoot, ec);
    if (ec) {
        return failAndExit(PZIP_E_IO, "failed to create output directory");
    }

    while (!m_canceled.load()) {
        uint32_t sig = 0;
        in.read(reinterpret_cast<char*>(&sig), sizeof(sig));
        if (in.eof()) {
            break;
        }
        if (!in.good()) {
            return failAndExit(PZIP_E_IO, "failed to read local file header signature");
        }

        if (sig == 0x02014b50U || sig == 0x06054b50U) {
            break;
        }
        if (sig != 0x04034b50U) {
            return failAndExit(PZIP_E_NOT_SUPPORTED, "unsupported zip structure");
        }

        uint16_t version = 0;
        uint16_t flags = 0;
        uint16_t method = 0;
        uint16_t dosTime = 0;
        uint16_t dosDate = 0;
        uint32_t crc32 = 0;
        uint32_t compressedSize = 0;
        uint32_t uncompressedSize = 0;
        uint16_t nameLen = 0;
        uint16_t extraLen = 0;

        in.read(reinterpret_cast<char*>(&version), sizeof(version));
        in.read(reinterpret_cast<char*>(&flags), sizeof(flags));
        in.read(reinterpret_cast<char*>(&method), sizeof(method));
        in.read(reinterpret_cast<char*>(&dosTime), sizeof(dosTime));
        in.read(reinterpret_cast<char*>(&dosDate), sizeof(dosDate));
        in.read(reinterpret_cast<char*>(&crc32), sizeof(crc32));
        in.read(reinterpret_cast<char*>(&compressedSize), sizeof(compressedSize));
        in.read(reinterpret_cast<char*>(&uncompressedSize), sizeof(uncompressedSize));
        in.read(reinterpret_cast<char*>(&nameLen), sizeof(nameLen));
        in.read(reinterpret_cast<char*>(&extraLen), sizeof(extraLen));
        if (!in.good()) {
            return failAndExit(PZIP_E_IO, "failed to read local file header");
        }

        if (compressedSize == 0xFFFFFFFFU || uncompressedSize == 0xFFFFFFFFU) {
            return failAndExit(PZIP_E_NOT_SUPPORTED, "ZIP64 archives are not supported");
        }

        if (static_cast<uint64_t>(uncompressedSize) > m_options.max_total_input_bytes) {
            return failAndExit(PZIP_E_NOT_SUPPORTED, "entry uncompressed size exceeds max allowed limit");
        }

        if (static_cast<uint64_t>(compressedSize) > m_options.max_total_input_bytes) {
            return failAndExit(PZIP_E_NOT_SUPPORTED, "entry compressed size exceeds max allowed limit");
        }

        if ((flags & 0x0008U) != 0U) {
            if (compressedSize == 0) {
                return failAndExit(PZIP_E_NOT_SUPPORTED, "streaming data descriptor zip is not supported yet");
            }
        }

        std::string entryName;
        entryName.resize(nameLen);
        if (nameLen > 0) {
            in.read(&entryName[0], static_cast<std::streamsize>(nameLen));
        }
        std::vector<uint8_t> extraField(extraLen);
        if (extraLen > 0) {
            in.read(reinterpret_cast<char*>(&extraField[0]), static_cast<std::streamsize>(extraLen));
        }
        if (!in.good()) {
            return failAndExit(PZIP_E_IO, "failed to read file name/extra field");
        }

        entryName = NormalizeEntryName(entryName);
        if (IsUnsafeEntry(entryName)) {
            return failAndExit(PZIP_E_NOT_SUPPORTED, "unsafe entry name in archive");
        }

        std::vector<uint8_t> payload;
        payload.resize(compressedSize);
        if (compressedSize > 0) {
            in.read(reinterpret_cast<char*>(&payload[0]), static_cast<std::streamsize>(compressedSize));
            if (!in.good()) {
                return failAndExit(PZIP_E_IO, "failed to read compressed payload");
            }
        }

        if ((flags & 0x0008U) != 0U) {
            uint32_t descSig = 0;
            in.read(reinterpret_cast<char*>(&descSig), sizeof(descSig));
            if (!in.good()) {
                return failAndExit(PZIP_E_IO, "failed to read data descriptor");
            }
            uint32_t descCrc = 0;
            uint32_t descComp = 0;
            uint32_t descUncomp = 0;
            if (descSig == 0x08074b50U) {
                in.read(reinterpret_cast<char*>(&descCrc), sizeof(descCrc));
                in.read(reinterpret_cast<char*>(&descComp), sizeof(descComp));
                in.read(reinterpret_cast<char*>(&descUncomp), sizeof(descUncomp));
            } else {
                descCrc = descSig;
                in.read(reinterpret_cast<char*>(&descComp), sizeof(descComp));
                in.read(reinterpret_cast<char*>(&descUncomp), sizeof(descUncomp));
            }
            if (!in.good()) {
                return failAndExit(PZIP_E_IO, "failed to read data descriptor fields");
            }
            if (crc32 == 0) {
                crc32 = descCrc;
            }
            if (uncompressedSize == 0) {
                uncompressedSize = descUncomp;
            }
        }

        std::vector<uint8_t> plain;
        const pzip_status_t decStatus = decompressAndDecryptPayload(
            method, flags, crc32, dosTime, uncompressedSize, extraField,
            std::move(payload), &plain);
        if (decStatus != PZIP_OK) {
            return failAndExit(decStatus, "failed to decompress or decrypt entry");
        }

        const fs::path outPath = outputRoot / fs::path(entryName);
        if (!entryName.empty() && entryName[entryName.size() - 1] == '/') {
            fs::create_directories(outPath, ec);
            if (ec) {
                return failAndExit(PZIP_E_IO, "failed to create directory entry");
            }
            continue;
        }

        const fs::path parent = outPath.parent_path();
        if (!parent.empty()) {
            fs::create_directories(parent, ec);
            if (ec) {
                return failAndExit(PZIP_E_IO, "failed to create entry parent directory");
            }
        }

        std::ofstream out(outPath.string().c_str(), std::ios::binary);
        if (!out.is_open()) {
            return failAndExit(PZIP_E_IO, "failed to create output file");
        }
        if (!plain.empty()) {
            out.write(reinterpret_cast<const char*>(&plain[0]), static_cast<std::streamsize>(plain.size()));
            if (!out.good()) {
                return failAndExit(PZIP_E_IO, "failed to write output file");
            }
        }

        if (m_progressCb != NULL) {
            m_progressCb(static_cast<uint64_t>(in.tellg()), totalArchiveBytes, m_progressUserData);
        }
    }

    if (m_canceled.load()) {
        return failAndExit(PZIP_E_CANCELED, "extract canceled");
    }

    if (m_progressCb != NULL) {
        m_progressCb(totalArchiveBytes, totalArchiveBytes, m_progressUserData);
    }

    setError(PZIP_OK, kOkMessage);
    {
        std::lock_guard<std::mutex> lock(m_mu);
        m_running = false;
    }
    return PZIP_OK;
}

pzip_status_t PzipEngine::cancel() {
    m_canceled.store(true);
    return PZIP_OK;
}

pzip_status_t PzipEngine::closeArchive() {
    std::lock_guard<std::mutex> lock(m_mu);
    if (m_running) {
        return PZIP_E_BUSY;
    }
    m_archivePath.clear();
    m_openMode = PZIP_MODE_CREATE;
    m_files.clear();
    m_existingRecords.clear();
    m_pendingRemovals.clear();
    m_pendingRenames.clear();
    m_canceled.store(false);
    return PZIP_OK;
}

pzip_status_t PzipEngine::extractEntry(const char* archivePath, const char* entryName,
                                       const char* outputFile) {
    if (archivePath == NULL || archivePath[0] == '\0' ||
        entryName == NULL || entryName[0] == '\0' ||
        outputFile == NULL || outputFile[0] == '\0') {
        return PZIP_E_INVALID_ARG;
    }

    {
        std::lock_guard<std::mutex> lock(m_mu);
        if (m_running) {
            return PZIP_E_BUSY;
        }
        m_running = true;
        m_canceled.store(false);
    }

    const auto failAndExit = [this](pzip_status_t status, const char* msg) {
        setError(status, msg);
        std::lock_guard<std::mutex> lock(m_mu);
        m_running = false;
        return status;
    };

    const std::string normEntry = NormalizeEntryName(EnsureUtf8(entryName));
    if (IsUnsafeEntry(normEntry)) {
        return failAndExit(PZIP_E_NOT_SUPPORTED, "unsafe entry name in archive");
    }

    ZipReader reader;
    const pzip_status_t openStatus = reader.open(EnsureUtf8(archivePath));
    if (openStatus != PZIP_OK) {
        return failAndExit(PZIP_E_IO, "failed to open archive for entry extraction");
    }

    std::vector<CentralRecord> records;
    const pzip_status_t cdStatus = reader.readCentralDirectory(&records);
    if (cdStatus != PZIP_OK) {
        return failAndExit(cdStatus, "failed to read archive central directory");
    }

    const CentralRecord* targetRec = NULL;
    for (size_t i = 0; i < records.size(); ++i) {
        if (NormalizeEntryName(records[i].m_name) == normEntry) {
            targetRec = &records[i];
            break;
        }
    }
    if (targetRec == NULL) {
        return failAndExit(PZIP_E_NOT_FOUND, "entry not found in archive");
    }

    const bool isDir = (!targetRec->m_name.empty() && targetRec->m_name[targetRec->m_name.size() - 1] == '/') ||
                       ((targetRec->m_externalFileAttr & 0x10U) != 0U) ||
                       (((targetRec->m_externalFileAttr >> 16U) & 0170000U) == 0040000U);

    const fs::path outFilePath(EnsureUtf8(outputFile));
    std::error_code ec;
    if (isDir) {
        fs::create_directories(outFilePath, ec);
        if (ec) {
            return failAndExit(PZIP_E_IO, "failed to create directory for entry");
        }
        std::lock_guard<std::mutex> lock(m_mu);
        m_running = false;
        return PZIP_OK;
    }

    const fs::path parent = outFilePath.parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent, ec);
        if (ec) {
            return failAndExit(PZIP_E_IO, "failed to create parent directory for entry");
        }
    }

    std::vector<uint8_t> payload;
    const pzip_status_t readStatus = reader.readEntryPayload(*targetRec, &payload);
    if (readStatus != PZIP_OK) {
        return failAndExit(readStatus, "failed to read compressed payload");
    }

    std::vector<uint8_t> plain;
    const pzip_status_t decStatus = decompressAndDecryptPayload(
        targetRec->m_method, targetRec->m_flags, targetRec->m_crc32,
        targetRec->m_dosTime, targetRec->m_uncompressedSize,
        targetRec->m_extraField, std::move(payload), &plain);
    if (decStatus != PZIP_OK) {
        return failAndExit(decStatus, "failed to decompress or decrypt entry");
    }

    std::ofstream out(outFilePath.string().c_str(), std::ios::binary);
    if (!out.is_open()) {
        return failAndExit(PZIP_E_IO, "failed to open output file for writing");
    }
    if (!plain.empty()) {
        out.write(reinterpret_cast<const char*>(&plain[0]),
                  static_cast<std::streamsize>(plain.size()));
        if (!out.good()) {
            return failAndExit(PZIP_E_IO, "failed to write decompressed data to file");
        }
    }
    out.close();

    std::lock_guard<std::mutex> lock(m_mu);
    m_running = false;
    return PZIP_OK;
}

pzip_status_t PzipEngine::decompressAndDecryptPayload(
    uint16_t method, uint16_t flags, uint32_t crc32, uint32_t dosTime,
    uint32_t uncompressedSize, const std::vector<uint8_t>& extraField,
    std::vector<uint8_t> payload, std::vector<uint8_t>* outPlain) const {
    if (outPlain == NULL) {
        return PZIP_E_INVALID_ARG;
    }

    const bool isEncrypted = (flags & 0x0001U) != 0U;
    bool isAe2 = false;
    if (isEncrypted) {
        if (m_password.empty()) {
            return PZIP_E_INVALID_ARG;
        }
        if (method == 99) {
            int32_t strength = 3;
            uint16_t actualMethod = 8;
            uint16_t vendorVer = 2;
            bool foundAesExtra = false;
            size_t offset = 0;
            while (offset + 4 <= extraField.size()) {
                const uint16_t headerId = static_cast<uint16_t>(
                    extraField[offset] | (extraField[offset + 1] << 8));
                const uint16_t dataSize = static_cast<uint16_t>(
                    extraField[offset + 2] | (extraField[offset + 3] << 8));
                offset += 4;
                if (offset + dataSize > extraField.size()) {
                    break;
                }
                if (headerId == 0x9901U && dataSize >= 7) {
                    vendorVer = static_cast<uint16_t>(
                        extraField[offset] | (extraField[offset + 1] << 8));
                    strength = static_cast<int32_t>(extraField[offset + 4]);
                    actualMethod = static_cast<uint16_t>(
                        extraField[offset + 5] | (extraField[offset + 6] << 8));
                    foundAesExtra = true;
                    break;
                }
                offset += dataSize;
            }
            if (!foundAesExtra) {
                return PZIP_E_NOT_SUPPORTED;
            }
            std::vector<uint8_t> decrypted;
            if (!crypto::WinZipAesDecrypt(m_password, strength,
                                          payload.empty() ? NULL : &payload[0],
                                          payload.size(), &decrypted)) {
                return PZIP_E_CODEC;
            }
            payload.swap(decrypted);
            method = actualMethod;
            isAe2 = (vendorVer == 2);
        } else {
            std::vector<uint8_t> decrypted;
            if (!crypto::ZipCryptoDecrypt(m_password, crc32, dosTime, (flags & 0x0008U) != 0U,
                                          payload.empty() ? NULL : &payload[0],
                                          payload.size(), &decrypted)) {
                return PZIP_E_CODEC;
            }
            payload.swap(decrypted);
        }
    }

    if (m_encryptionEnabled) {
        if (m_encryption.m_vtable.decrypt == NULL) {
            return PZIP_E_INVALID_ARG;
        }
        size_t dstCap = payload.size();
        if (m_encryption.m_vtable.bound != NULL) {
            dstCap = m_encryption.m_vtable.bound(m_encryption.m_instance, payload.size());
        }
        if (dstCap < payload.size()) {
            dstCap = payload.size();
        }

        std::vector<uint8_t> decrypted(dstCap);
        size_t dstSize = 0;
        const pzip_status_t decStatus =
            m_encryption.m_vtable.decrypt(m_encryption.m_instance,
                                          payload.empty() ? NULL : &payload[0],
                                          payload.size(),
                                          decrypted.empty() ? NULL : &decrypted[0],
                                          decrypted.size(),
                                          &dstSize);
        if (decStatus != PZIP_OK || dstSize > decrypted.size()) {
            return PZIP_E_CODEC;
        }
        decrypted.resize(dstSize);
        payload.swap(decrypted);
    }

    std::vector<uint8_t> plain;
    if (method == 0) {
        plain.swap(payload);
    } else {
        pzip_codec_vtable_t tempVtable;
        bool useTempCodec = false;
        void* tempInstance = NULL;

        const pzip_codec_vtable_t* activeVtable = &m_codec.m_vtable;
        void* activeInstance = m_codec.m_instance;

        if (activeVtable->decompress == NULL || activeVtable->zip_method != method) {
            if (method == 8) {
                tempVtable = codec::CreateZlibCodecVtable();
                useTempCodec = true;
            } else if (method == 93) {
                tempVtable = codec::CreateZstdCodecVtable();
                useTempCodec = true;
            } else if (method == 94) {
                tempVtable = codec::CreateLz4CodecVtable();
                useTempCodec = true;
            } else {
                return PZIP_E_NOT_SUPPORTED;
            }
            if (tempVtable.create != NULL) {
                tempInstance = tempVtable.create(NULL);
            }
            activeVtable = &tempVtable;
            activeInstance = tempInstance;
        }

        size_t dstCap = uncompressedSize == 0 ? 1 : uncompressedSize;
        plain.resize(dstCap);
        size_t dstSize = 0;
        const pzip_status_t decStatus =
            activeVtable->decompress(activeInstance,
                                     payload.empty() ? NULL : &payload[0],
                                     payload.size(),
                                     plain.empty() ? NULL : &plain[0],
                                     plain.size(),
                                     &dstSize);
        if (useTempCodec && tempVtable.destroy != NULL && tempInstance != NULL) {
            tempVtable.destroy(tempInstance);
        }
        if (decStatus != PZIP_OK || dstSize > plain.size()) {
            return PZIP_E_CODEC;
        }
        plain.resize(dstSize);
    }

    if (uncompressedSize != static_cast<uint32_t>(plain.size())) {
        return PZIP_E_CODEC;
    }
    if (!isAe2 && Crc32(plain.empty() ? NULL : &plain[0], plain.size()) != crc32) {
        return PZIP_E_CODEC;
    }

    *outPlain = std::move(plain);
    return PZIP_OK;
}

int32_t PzipEngine::lastErrorCode() const {
    std::lock_guard<std::mutex> lock(m_errorMu);
    return m_lastErrorCode;
}

const char* PzipEngine::lastErrorMessage() const {
    std::lock_guard<std::mutex> lock(m_errorMu);
    return m_lastErrorMessage.c_str();
}

void PzipEngine::setError(int32_t code, const std::string& message) const {
    std::lock_guard<std::mutex> lock(m_errorMu);
    m_lastErrorCode = code;
    m_lastErrorMessage = message;
}

std::string PzipEngine::NormalizeEntryName(std::string name) {
    std::replace(name.begin(), name.end(), '\\', '/');
    const size_t first = name.find_first_not_of('/');
    if (first == std::string::npos) {
        return "";
    }
    if (first > 0) {
        name.erase(0, first);
    }
    return name;
}

bool PzipEngine::IsUnsafeEntry(const std::string& name) {
    if (name.empty()) {
        return true;
    }
    if (name.size() > 1 && name[1] == ':') {
        return true;
    }
    size_t start = 0;
    while (start < name.size()) {
        size_t end = name.find('/', start);
        if (end == std::string::npos) {
            end = name.size();
        }
        if (end - start == 2 && name[start] == '.' && name[start + 1] == '.') {
            return true;
        }
        start = end + 1;
    }
    return false;
}

std::string PzipEngine::MakeRelativeEntry(const fs::path& path, const fs::path& base) {
    std::string full = path.generic_u8string();
    std::string root = base.generic_u8string();
    if (!root.empty() && root[root.size() - 1] != '/') {
        root.push_back('/');
    }
    if (full.compare(0, root.size(), root) == 0) {
        return full.substr(root.size());
    }
    return path.filename().generic_u8string();
}

uint32_t PzipEngine::Crc32(const uint8_t* data, size_t size) {
    if (size == 0 || data == NULL) {
        return 0;
    }

    uLong crc = ::crc32(0L, Z_NULL, 0);
    size_t offset = 0;
    while (offset < size) {
        const size_t remaining = size - offset;
        const uInt chunk = static_cast<uInt>(
            (std::min<size_t>)(remaining, static_cast<size_t>(std::numeric_limits<uInt>::max())));
        crc = ::crc32(crc, reinterpret_cast<const Bytef*>(data + offset), chunk);
        offset += static_cast<size_t>(chunk);
    }
    return static_cast<uint32_t>(crc);
}

pzip_status_t PzipEngine::compressChunkTask(ChunkTask* chunkTask, WriteTask* writeTask,
                                            void* localCodecCtx) const {
    if (chunkTask == NULL || writeTask == NULL || chunkTask->m_fileIndex >= m_files.size()) {
        return PZIP_E_INVALID_ARG;
    }

    OutputFile* outFile = &writeTask->m_output;
    writeTask->m_fileIndex = chunkTask->m_fileIndex;
    outFile->m_entryName = m_files[chunkTask->m_fileIndex].m_entryName;
    outFile->m_uncompressedSize = static_cast<uint32_t>(chunkTask->m_rawData.size() & 0xffffffffU);
    outFile->m_crc32 = Crc32(chunkTask->m_rawData.empty() ? NULL : &chunkTask->m_rawData[0],
                             chunkTask->m_rawData.size());
    m_timeProvider.fillDosDateTime(&outFile->m_dosTime, &outFile->m_dosDate);

    if (m_codec.m_vtable.compress == NULL || (m_codec.m_vtable.create != NULL && localCodecCtx == NULL)) {
        outFile->m_method = 0;
        outFile->m_payload.swap(chunkTask->m_rawData);
        return PZIP_OK;
    }

    if ((m_codec.m_vtable.flags & PZIP_CODEC_FLAG_ZIP_COMPATIBLE) == 0U) {
        return PZIP_E_NOT_SUPPORTED;
    }

    size_t dstCap = chunkTask->m_rawData.size();
    if (m_codec.m_vtable.bound != NULL) {
        dstCap = m_codec.m_vtable.bound(localCodecCtx, chunkTask->m_rawData.size());
    }
    if (dstCap == 0) {
        dstCap = 1;
    }

    std::vector<uint8_t> compressed(dstCap);
    size_t dstSize = 0;
    pzip_status_t status =
        m_codec.m_vtable.compress(localCodecCtx,
                                  chunkTask->m_rawData.empty() ? NULL : &chunkTask->m_rawData[0],
                                  chunkTask->m_rawData.size(),
                                  compressed.empty() ? NULL : &compressed[0],
                                  compressed.size(),
                                  &dstSize);
    if (status != PZIP_OK) {
        return PZIP_E_CODEC;
    }

    if (dstSize >= chunkTask->m_rawData.size()) {
        outFile->m_method = 0;
        outFile->m_payload.swap(chunkTask->m_rawData);
        return PZIP_OK;
    }

    compressed.resize(dstSize);
    outFile->m_method = m_codec.m_vtable.zip_method;
    outFile->m_payload.swap(compressed);
    return PZIP_OK;
}

}  // namespace pzip
