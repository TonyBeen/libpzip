#ifndef PZIP_CORE_PZIP_ENGINE_H_
#define PZIP_CORE_PZIP_ENGINE_H_

#include <stdint.h>

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "ghc/filesystem.hpp"
#include "io/file_reader.h"
#include "platform/time_provider.h"
#include "pzip.h"
#include "zip/zip_writer.h"

namespace pzip {

namespace fs = ghc::filesystem;

struct PendingFile {
    std::string m_diskPath;
    std::string m_entryName;
    uint64_t m_fileSize;

    PendingFile() : m_fileSize(0) {}
};

class PzipEngine {
   public:
    PzipEngine();
    ~PzipEngine();

    pzip_status_t configure(const pzip_options_t* options);
    pzip_status_t setCodec(const pzip_codec_vtable_t* codec, void* codecUser);
    pzip_status_t setEncryption(const pzip_encryption_vtable_t* encryption, void* encryptionUser);
    pzip_status_t setEncryptionEnabled(uint32_t enabled);
    pzip_status_t setEncryptionConfig(const pzip_encryption_config_t* config);
    pzip_status_t getEncryptionConfig(pzip_encryption_config_t* outConfig) const;

    pzip_status_t openArchive(const char* archivePath);
    pzip_status_t openArchiveMode(const char* archivePath, pzip_open_mode_t mode);
    pzip_status_t addPath(const char* srcPath, const char* entryPrefix);
    pzip_status_t removeEntry(const char* entryName, int32_t recursive);
    pzip_status_t renameEntry(const char* oldEntryName, const char* newEntryName);
    pzip_status_t hasEntry(const char* entryName, int32_t* outExists) const;
    pzip_status_t statEntry(const char* entryName, pzip_entry_info_t* outInfo) const;
    pzip_status_t getEntryCount(uint32_t* outCount) const;
    pzip_status_t listEntries(pzip_list_callback_t callback, void* userData) const;
    pzip_status_t extractEntry(const char* archivePath, const char* entryName,
                               const char* outputFile);
    pzip_status_t clearPending();
    pzip_status_t run();
    pzip_status_t extractArchive(const char* archivePath, const char* outputDir);
    pzip_status_t cancel();
    pzip_status_t closeArchive();
    pzip_status_t setProgressCallback(pzip_progress_callback_t callback, void* userData);
    pzip_status_t setPassword(const char* password);
    pzip_status_t setCryptoMode(pzip_crypto_mode_t mode);

    int32_t lastErrorCode() const;
    const char* lastErrorMessage() const;

   private:
    struct CodecState {
        pzip_codec_vtable_t m_vtable;
        void* m_user;
        void* m_instance;

        CodecState() : m_user(NULL), m_instance(NULL) {}
    };

    struct EncryptionState {
        pzip_encryption_vtable_t m_vtable;
        void* m_user;
        void* m_instance;

        EncryptionState() : m_user(NULL), m_instance(NULL) {}
    };

    struct FileTask {
        size_t m_fileIndex;
    };

    struct ChunkTask {
        size_t m_fileIndex;
        size_t m_chunkIndex;
        size_t m_totalChunks;
        std::vector<uint8_t> m_rawData;

        ChunkTask() : m_fileIndex(0), m_chunkIndex(0), m_totalChunks(0) {}
    };

    struct WriteTask {
        size_t m_fileIndex;
        OutputFile m_output;
    };

    static std::string NormalizeEntryName(std::string name);
    static bool IsUnsafeEntry(const std::string& name);
    static std::string MakeRelativeEntry(const fs::path& path, const fs::path& base);
    static uint32_t Crc32(const uint8_t* data, size_t size);
    pzip_status_t compressChunkTask(ChunkTask* chunkTask, WriteTask* writeTask,
                                    void* localCodecCtx) const;

    pzip_status_t validateReadyForRun() const;
    void setError(int32_t code, const std::string& message) const;

    pzip_status_t pushSingleFile(const fs::path& src, const std::string& prefix, uint64_t fileSize);

    struct PendingRemoval {
        std::string m_pattern;
        int32_t m_recursive;
    };

    bool isEntryRemoved(const std::string& entryName) const;
    std::string getRenamedName(const std::string& entryName) const;
    pzip_status_t decompressAndDecryptPayload(
        uint16_t method, uint16_t flags, uint32_t crc32, uint32_t dosTime,
        uint32_t uncompressedSize, const std::vector<uint8_t>& extraField,
        std::vector<uint8_t> payload, std::vector<uint8_t>* outPlain) const;

    mutable std::mutex m_mu;
    mutable std::mutex m_errorMu;
    mutable int32_t m_lastErrorCode;
    mutable std::string m_lastErrorMessage;

    pzip_options_t m_options;
    pzip_open_mode_t m_openMode;
    std::string m_archivePath;
    std::vector<PendingFile> m_files;
    std::vector<CentralRecord> m_existingRecords;
    std::vector<PendingRemoval> m_pendingRemovals;
    std::map<std::string, std::string> m_pendingRenames;
    mutable std::string m_lastStatEntryName;

    CodecState m_codec;
    EncryptionState m_encryption;
    pzip_encryption_config_t m_encryptionConfig;
    bool m_encryptionEnabled;
    std::atomic<bool> m_canceled;
    bool m_running;

    pzip_progress_callback_t m_progressCb;
    void* m_progressUserData;

    std::string m_password;
    pzip_crypto_mode_t m_cryptoMode;

    io::FileReader m_fileReader;
    platform::TimeProvider m_timeProvider;
};

}  // namespace pzip

#endif  // PZIP_CORE_PZIP_ENGINE_H_
