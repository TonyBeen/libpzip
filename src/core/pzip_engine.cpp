#include "core/pzip_engine.h"

#include <algorithm>
#include <atomic>
#include <cctype>
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

#include "codec/zstd_codec.h"
#include "codec/zlib_codec.h"
#include "zip/zip_writer.h"

namespace pzip {

namespace fs = ghc::filesystem;

namespace {
static const char* kOkMessage = "OK";
static const uint32_t kDefaultChunkSizeKb = 256U;
static const uint32_t kDefaultMaxFileCount = 1000000U;
static const uint64_t kDefaultMaxInputBytes = 1ULL << 40;

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
      m_encryptionEnabled(false),
      m_canceled(false),
      m_running(false) {
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
    if (archivePath == NULL || archivePath[0] == '\0') {
        return PZIP_E_INVALID_ARG;
    }

    std::lock_guard<std::mutex> lock(m_mu);
    if (m_running) {
        return PZIP_E_BUSY;
    }

    m_archivePath = archivePath;
    m_files.clear();
    m_canceled.store(false);
    setError(PZIP_OK, kOkMessage);
    return PZIP_OK;
}

pzip_status_t PzipEngine::pushSingleFile(const fs::path& src, const std::string& prefix) {
    std::string entry = prefix.empty() ? src.filename().generic_string()
                                       : prefix + "/" + src.filename().generic_string();
    entry = NormalizeEntryName(entry);
    if (IsUnsafeEntry(entry)) {
        return PZIP_E_INVALID_ARG;
    }
    if (m_files.size() >= m_options.max_file_count) {
        return PZIP_E_NOT_SUPPORTED;
    }

    PendingFile pending;
    pending.m_diskPath = src.string();
    pending.m_entryName = entry;
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

    const fs::path src(srcPath);
    if (!fs::exists(src)) {
        return PZIP_E_NOT_FOUND;
    }

    const std::string prefix = entryPrefix == NULL ? "" : NormalizeEntryName(entryPrefix);

    if (fs::is_regular_file(src)) {
        return pushSingleFile(src, prefix);
    }

    uint64_t totalBytes = 0;
    for (fs::recursive_directory_iterator it(src); it != fs::recursive_directory_iterator(); ++it) {
        const fs::path item = it->path();
        if (!fs::is_regular_file(item)) {
            continue;
        }

        totalBytes += static_cast<uint64_t>(fs::file_size(item));
        if (totalBytes > m_options.max_total_input_bytes) {
            return PZIP_E_NOT_SUPPORTED;
        }

        std::string rel = MakeRelativeEntry(item, src);
        std::string entry = prefix.empty() ? rel : prefix + "/" + rel;
        entry = NormalizeEntryName(entry);
        if (IsUnsafeEntry(entry)) {
            return PZIP_E_INVALID_ARG;
        }
        if (m_files.size() >= m_options.max_file_count) {
            return PZIP_E_NOT_SUPPORTED;
        }

        PendingFile pending;
        pending.m_diskPath = item.string();
        pending.m_entryName = entry;
        m_files.push_back(pending);
    }

    return PZIP_OK;
}

pzip_status_t PzipEngine::validateReadyForRun() const {
    if (m_archivePath.empty()) {
        return PZIP_E_INVALID_ARG;
    }
    if (m_files.empty()) {
        return PZIP_E_INVALID_ARG;
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

    const uint32_t cpuThreads = std::max(1U, std::thread::hardware_concurrency());
    const uint32_t defaultThreads = std::min(16U, cpuThreads);
    const uint32_t threadCount = m_options.thread_count == 0 ? defaultThreads : m_options.thread_count;
    const size_t chunkBytes =
        std::max<size_t>(1, static_cast<size_t>(m_options.chunk_size_kb) * static_cast<size_t>(1024));
    const size_t queueCapacity = std::max<size_t>(4, static_cast<size_t>(threadCount) * 2);
    const size_t defaultPendingBytesLimit =
        std::max<size_t>(chunkBytes * static_cast<size_t>(threadCount) * 4, chunkBytes * 8);
    const size_t pendingBytesLimit = std::max<size_t>(chunkBytes, std::min<size_t>(
                                                                  defaultPendingBytesLimit,
                                                                  static_cast<size_t>(m_options.max_total_input_bytes)));
    const size_t pendingShardCount = std::max<size_t>(1, std::min<size_t>(static_cast<size_t>(threadCount), 8));
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
                const pzip_status_t status = compressChunkTask(&mergedChunk, &writeTask);
                if (status != PZIP_OK) {
                    markFailed(status);
                    break;
                }
                reusableMergeBuffer.swap(mergedChunk.m_rawData);
                if (!writeQueue.push(std::move(writeTask))) {
                    break;
                }
            }
            if (aliveWorkers.fetch_sub(1) == 1) {
                writeQueue.close();
            }
        }));
    }

    std::thread writerThread([this, &writeQueue, &markFailed]() {
        ZipWriter writer(m_archivePath);
        if (!writer.isOpen()) {
            markFailed(PZIP_E_IO);
            writeQueue.close();
            return;
        }

        std::vector<CentralRecord> records;
        records.reserve(m_files.size());
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

                CentralRecord rec;
                if (!writer.writeEntry(pendingOutputs[expectedIndex], &rec)) {
                    markFailed(PZIP_E_IO);
                    writeQueue.close();
                    return;
                }
                records.push_back(rec);
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
        }
    });

    reader.join();
    for (size_t i = 0; i < workers.size(); ++i) {
        workers[i].join();
    }
    writerThread.join();

    if (m_canceled.load()) {
        const int32_t err = firstError.load() == PZIP_OK ? PZIP_E_CANCELED : firstError.load();
        setError(err, "compression canceled or failed");
        std::lock_guard<std::mutex> lock(m_mu);
        m_running = false;
        return static_cast<pzip_status_t>(err);
    }

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

    std::ifstream in(archivePath, std::ios::binary);
    if (!in.is_open()) {
        return failAndExit(PZIP_E_IO, "failed to open archive for extract");
    }

    const fs::path outputRoot(outputDir);
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

        if ((flags & 0x0008U) != 0U) {
            return failAndExit(PZIP_E_NOT_SUPPORTED, "data descriptor zip is not supported yet");
        }

        std::string entryName;
        entryName.resize(nameLen);
        if (nameLen > 0) {
            in.read(&entryName[0], static_cast<std::streamsize>(nameLen));
        }
        if (extraLen > 0) {
            in.seekg(static_cast<std::streamoff>(extraLen), std::ios::cur);
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

        if (m_encryptionEnabled) {
            if (m_encryption.m_vtable.decrypt == NULL) {
                return failAndExit(PZIP_E_INVALID_ARG,
                                   "encryption enabled but decrypt callback is not set");
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
                return failAndExit(PZIP_E_CODEC, "failed to decrypt payload");
            }
            decrypted.resize(dstSize);
            payload.swap(decrypted);
        }

        std::vector<uint8_t> plain;
        if (method == 0) {
            plain.swap(payload);
        } else {
            if (m_codec.m_vtable.decompress == NULL || m_codec.m_instance == NULL ||
                m_codec.m_vtable.zip_method != method) {
                return failAndExit(PZIP_E_NOT_SUPPORTED, "unsupported compression method for extract");
            }

            size_t dstCap = uncompressedSize == 0 ? payload.size() * 4 + 1 : uncompressedSize;
            if (dstCap == 0) {
                dstCap = 1;
            }
            plain.resize(dstCap);
            size_t dstSize = 0;
            const pzip_status_t decStatus =
                m_codec.m_vtable.decompress(m_codec.m_instance,
                                            payload.empty() ? NULL : &payload[0],
                                            payload.size(),
                                            plain.empty() ? NULL : &plain[0],
                                            plain.size(),
                                            &dstSize);
            if (decStatus != PZIP_OK || dstSize > plain.size()) {
                return failAndExit(PZIP_E_CODEC, "failed to decompress payload");
            }
            plain.resize(dstSize);
        }

        if (uncompressedSize != static_cast<uint32_t>(plain.size())) {
            return failAndExit(PZIP_E_CODEC, "uncompressed size mismatch");
        }
        if (Crc32(plain.empty() ? NULL : &plain[0], plain.size()) != crc32) {
            return failAndExit(PZIP_E_CODEC, "crc32 mismatch");
        }

        const fs::path outPath = outputRoot / fs::path(entryName);
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
    }

    if (m_canceled.load()) {
        return failAndExit(PZIP_E_CANCELED, "extract canceled");
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
    m_files.clear();
    m_canceled.store(false);
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
    while (!name.empty() && name[0] == '/') {
        name.erase(name.begin());
    }
    return name;
}

bool PzipEngine::IsUnsafeEntry(const std::string& name) {
    if (name.empty()) {
        return true;
    }
    if (name.find("..") != std::string::npos) {
        return true;
    }
    if (name.size() > 2 && std::isalpha(static_cast<unsigned char>(name[0])) && name[1] == ':') {
        return true;
    }
    return false;
}

std::string PzipEngine::MakeRelativeEntry(const fs::path& path, const fs::path& base) {
    std::string full = path.generic_string();
    std::string root = base.generic_string();
    if (!root.empty() && root[root.size() - 1] != '/') {
        root.push_back('/');
    }
    if (full.compare(0, root.size(), root) == 0) {
        return full.substr(root.size());
    }
    return path.filename().generic_string();
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
            std::min<size_t>(remaining, static_cast<size_t>(std::numeric_limits<uInt>::max())));
        crc = ::crc32(crc, reinterpret_cast<const Bytef*>(data + offset), chunk);
        offset += static_cast<size_t>(chunk);
    }
    return static_cast<uint32_t>(crc);
}

pzip_status_t PzipEngine::compressChunkTask(ChunkTask* chunkTask, WriteTask* writeTask) const {
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

    if (m_codec.m_vtable.compress == NULL || m_codec.m_instance == NULL) {
        outFile->m_method = 0;
        outFile->m_payload.swap(chunkTask->m_rawData);
        return PZIP_OK;
    }

    if ((m_codec.m_vtable.flags & PZIP_CODEC_FLAG_ZIP_COMPATIBLE) == 0U) {
        return PZIP_E_NOT_SUPPORTED;
    }

    size_t dstCap = chunkTask->m_rawData.size();
    if (m_codec.m_vtable.bound != NULL) {
        dstCap = m_codec.m_vtable.bound(m_codec.m_instance, chunkTask->m_rawData.size());
    }
    if (dstCap == 0) {
        dstCap = 1;
    }

    std::vector<uint8_t> compressed(dstCap);
    size_t dstSize = 0;
    pzip_status_t status =
        m_codec.m_vtable.compress(m_codec.m_instance,
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

pzip_status_t PzipEngine::compressOne(size_t index, std::vector<OutputFile>* outputs) const {
    if (outputs == NULL || index >= m_files.size()) {
        return PZIP_E_INVALID_ARG;
    }

    std::vector<uint8_t> raw;
    pzip_status_t status = m_fileReader.readWholeFile(m_files[index].m_diskPath, &raw);
    if (status != PZIP_OK) {
        return status;
    }

    OutputFile* outFile = &(*outputs)[index];
    outFile->m_entryName = m_files[index].m_entryName;
    outFile->m_uncompressedSize = static_cast<uint32_t>(raw.size());
    outFile->m_crc32 = Crc32(raw.empty() ? NULL : &raw[0], raw.size());
    m_timeProvider.fillDosDateTime(&outFile->m_dosTime, &outFile->m_dosDate);

    if (m_codec.m_vtable.compress == NULL || m_codec.m_instance == NULL) {
        outFile->m_method = 0;
        outFile->m_payload.swap(raw);
        return PZIP_OK;
    }

    if ((m_codec.m_vtable.flags & PZIP_CODEC_FLAG_ZIP_COMPATIBLE) == 0U) {
        return PZIP_E_NOT_SUPPORTED;
    }

    size_t dstCap = raw.size();
    if (m_codec.m_vtable.bound != NULL) {
        dstCap = m_codec.m_vtable.bound(m_codec.m_instance, raw.size());
    }
    if (dstCap == 0) {
        dstCap = 1;
    }

    std::vector<uint8_t> compressed(dstCap);
    size_t dstSize = 0;
    status = m_codec.m_vtable.compress(m_codec.m_instance, raw.empty() ? NULL : &raw[0], raw.size(),
                                       compressed.empty() ? NULL : &compressed[0],
                                       compressed.size(), &dstSize);
    if (status != PZIP_OK) {
        return PZIP_E_CODEC;
    }

    if (dstSize >= raw.size()) {
        outFile->m_method = 0;
        outFile->m_payload.swap(raw);
        return PZIP_OK;
    }

    compressed.resize(dstSize);
    outFile->m_method = m_codec.m_vtable.zip_method;
    outFile->m_payload.swap(compressed);
    return PZIP_OK;
}

}  // namespace pzip
