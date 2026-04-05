#ifndef PZIP_CORE_PZIP_ENGINE_H_
#define PZIP_CORE_PZIP_ENGINE_H_

#include <stdint.h>

#include <atomic>
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
    pzip_status_t addPath(const char* srcPath, const char* entryPrefix);
    pzip_status_t run();
    pzip_status_t extractArchive(const char* archivePath, const char* outputDir);
    pzip_status_t cancel();
    pzip_status_t closeArchive();

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
    pzip_status_t compressOne(size_t index, std::vector<class OutputFile>* outputs) const;
    pzip_status_t compressChunkTask(ChunkTask* chunkTask, WriteTask* writeTask) const;

    pzip_status_t validateReadyForRun() const;
    void setError(int32_t code, const std::string& message) const;

    pzip_status_t pushSingleFile(const fs::path& src, const std::string& prefix);

    mutable std::mutex m_mu;
    mutable std::mutex m_errorMu;
    mutable int32_t m_lastErrorCode;
    mutable std::string m_lastErrorMessage;

    pzip_options_t m_options;
    std::string m_archivePath;
    std::vector<PendingFile> m_files;
    CodecState m_codec;
    EncryptionState m_encryption;
    pzip_encryption_config_t m_encryptionConfig;
    bool m_encryptionEnabled;
    std::atomic<bool> m_canceled;
    bool m_running;

    io::FileReader m_fileReader;
    platform::TimeProvider m_timeProvider;
};

}  // namespace pzip

#endif  // PZIP_CORE_PZIP_ENGINE_H_
