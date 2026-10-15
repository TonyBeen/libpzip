#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <direct.h>
#else
#include <unistd.h>
#endif

#include "pzip.h"

namespace {

std::atomic<int> g_createCount(0);
std::atomic<int> g_destroyCount(0);

struct StatefulCodecContext {
    int worker_id;
    int chunk_counter;
};

void* StatefulCodecCreate(void*) {
    int id = g_createCount.fetch_add(1);
    StatefulCodecContext* ctx = new StatefulCodecContext();
    ctx->worker_id = id;
    ctx->chunk_counter = 0;
    return ctx;
}

void StatefulCodecDestroy(void* codecCtx) {
    g_destroyCount.fetch_add(1);
    delete static_cast<StatefulCodecContext*>(codecCtx);
}

pzip_status_t StatefulCodecCompress(void* codecCtx, const uint8_t* src, size_t srcSize,
                                    uint8_t* dst, size_t dstCap, size_t* dstSize) {
    StatefulCodecContext* ctx = static_cast<StatefulCodecContext*>(codecCtx);
    assert(ctx != NULL);
    ctx->chunk_counter++;

    if (dstSize == NULL) return PZIP_E_INVALID_ARG;
    if (dstCap < srcSize) return PZIP_E_NO_MEMORY;
    if (srcSize > 0 && src != NULL && dst != NULL) {
        std::memcpy(dst, src, srcSize);
    }
    *dstSize = srcSize;
    return PZIP_OK;
}

pzip_status_t StatefulCodecDecompress(void*, const uint8_t* src, size_t srcSize,
                                      uint8_t* dst, size_t dstCap, size_t* dstSize) {
    if (dstSize == NULL) return PZIP_E_INVALID_ARG;
    if (dstCap < srcSize) return PZIP_E_NO_MEMORY;
    if (srcSize > 0 && src != NULL && dst != NULL) {
        std::memcpy(dst, src, srcSize);
    }
    *dstSize = srcSize;
    return PZIP_OK;
}

size_t StatefulCodecBound(void*, size_t srcSize) {
    return srcSize + 16;
}

const char* StatefulCodecName(void*) {
    return "stateful-test";
}

bool WriteTestFile(const char* path, const char* content) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    size_t len = std::strlen(content);
    size_t written = std::fwrite(content, 1, len, f);
    std::fclose(f);
    return written == len;
}

bool ReadWholeFile(const char* path, std::vector<uint8_t>* out) {
    FILE* in = std::fopen(path, "rb");
    if (!in || !out) return false;
    if (std::fseek(in, 0, SEEK_END) != 0) {
        std::fclose(in);
        return false;
    }
    long sz = std::ftell(in);
    if (sz < 0 || sz > 100 * 1024 * 1024) {
        std::fclose(in);
        return false;
    }
    std::fseek(in, 0, SEEK_SET);
    out->resize(static_cast<size_t>(sz));
    if (sz > 0 && std::fread(&(*out)[0], 1, static_cast<size_t>(sz), in) != static_cast<size_t>(sz)) {
        std::fclose(in);
        return false;
    }
    std::fclose(in);
    return true;
}

struct ProgressRecord {
    int callCount;
    uint64_t lastProcessed;
    uint64_t totalBytes;
    ProgressRecord() : callCount(0), lastProcessed(0), totalBytes(0) {}
};

void OnProgress(uint64_t processed, uint64_t total, void* userData) {
    ProgressRecord* rec = static_cast<ProgressRecord*>(userData);
    assert(rec != NULL);
    rec->callCount++;
    rec->lastProcessed = processed;
    rec->totalBytes = total;
    assert(processed <= total);
}

uint32_t SimpleCrc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k) {
            crc = (crc >> 1) ^ (0xEDB88320U & (-(crc & 1)));
        }
    }
    return ~crc;
}

bool TestDataDescriptorExtract(bool withSignature) {
    const char* zipName = withSignature ? "test_data_desc_sig.zip" : "test_data_desc_nosig.zip";
    const char* outDir = withSignature ? "test_desc_out_sig" : "test_desc_out_nosig";
    const char* content = "Data descriptor test: verifying bit 3 stream payload\n";
    size_t contentLen = std::strlen(content);
    uint32_t crc = SimpleCrc32(reinterpret_cast<const uint8_t*>(content), contentLen);
    const char* entryName = "desc_file.txt";
    uint16_t nameLen = static_cast<uint16_t>(std::strlen(entryName));

    std::vector<uint8_t> zipBytes;
    auto appendU16 = [&zipBytes](uint16_t val) {
        zipBytes.push_back(static_cast<uint8_t>(val & 0xFF));
        zipBytes.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    };
    auto appendU32 = [&zipBytes](uint32_t val) {
        zipBytes.push_back(static_cast<uint8_t>(val & 0xFF));
        zipBytes.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
        zipBytes.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
        zipBytes.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
    };
    auto appendBytes = [&zipBytes](const void* data, size_t len) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(data);
        zipBytes.insert(zipBytes.end(), p, p + len);
    };

    // 1. Local File Header
    appendU32(0x04034b50U); // signature
    appendU16(20);          // version needed
    appendU16(0x0808);      // flags: Bit 3 (data descriptor) + Bit 11 (UTF-8)
    appendU16(0);           // method: store (0)
    appendU16(0);           // time
    appendU16(0);           // date
    appendU32(0);           // crc32 in local header is 0 when bit 3 is set
    appendU32(static_cast<uint32_t>(contentLen)); // compressed size
    appendU32(0);           // uncompressed size is 0 in local header when bit 3 is set
    appendU16(nameLen);
    appendU16(0);           // extraLen
    appendBytes(entryName, nameLen);
    appendBytes(content, contentLen);

    // 2. Data Descriptor
    if (withSignature) {
        appendU32(0x08074b50U); // optional signature
    }
    appendU32(crc);
    appendU32(static_cast<uint32_t>(contentLen));
    appendU32(static_cast<uint32_t>(contentLen));

    // 3. Central Directory Header
    uint32_t cdStart = static_cast<uint32_t>(zipBytes.size());
    appendU32(0x02014b50U); // central dir signature
    appendU16(20);          // version made by
    appendU16(20);          // version needed
    appendU16(0x0808);      // flags
    appendU16(0);           // method
    appendU16(0);           // time
    appendU16(0);           // date
    appendU32(crc);
    appendU32(static_cast<uint32_t>(contentLen));
    appendU32(static_cast<uint32_t>(contentLen));
    appendU16(nameLen);
    appendU16(0);           // extraLen
    appendU16(0);           // commentLen
    appendU16(0);           // diskNum
    appendU16(0);           // internalAttr
    appendU32(0);           // externalAttr
    appendU32(0);           // localHeaderOffset
    appendBytes(entryName, nameLen);

    uint32_t cdSize = static_cast<uint32_t>(zipBytes.size()) - cdStart;

    // 4. End of Central Directory
    appendU32(0x06054b50U); // EOCD signature
    appendU16(0);           // disk number
    appendU16(0);           // start disk
    appendU16(1);           // entries on disk
    appendU16(1);           // total entries
    appendU32(cdSize);
    appendU32(cdStart);
    appendU16(0);           // comment length

    FILE* f = std::fopen(zipName, "wb");
    if (!f) return false;
    std::fwrite(zipBytes.data(), 1, zipBytes.size(), f);
    std::fclose(f);

    pzip_ctx_t* extractCtx = NULL;
    pzip_options_t opt;
    std::memset(&opt, 0, sizeof(opt));
    opt.abi_version = PZIP_ABI_VERSION;
    opt.thread_count = 1;
    if (pzip_create(&opt, &extractCtx) != PZIP_OK) {
        return false;
    }

    pzip_status_t status = pzip_extract_archive(extractCtx, zipName, outDir);
    pzip_destroy(extractCtx);

    if (status != PZIP_OK) {
        std::fprintf(stderr, "Extract data descriptor zip failed: %d\n", status);
        return false;
    }

    // Verify extracted content
    std::string extractedFile = std::string(outDir) + "/" + entryName;
    std::vector<uint8_t> outBytes;
    if (!ReadWholeFile(extractedFile.c_str(), &outBytes) ||
        outBytes.size() != contentLen ||
        std::memcmp(outBytes.data(), content, contentLen) != 0) {
        return false;
    }

    // Cleanup
    std::remove(extractedFile.c_str());
#if defined(_WIN32)
    _rmdir(outDir);
#else
    rmdir(outDir);
#endif
    std::remove(zipName);
    return true;
}

}  // namespace

int main() {
    // 1. Create a dummy file
    const char* testFile = "pzip_edge_test_file.txt";
    if (!WriteTestFile(testFile, "hello security edge test content\n")) {
        return 1;
    }

    pzip_ctx_t* ctx = NULL;
    pzip_options_t opt;
    std::memset(&opt, 0, sizeof(opt));
    opt.abi_version = PZIP_ABI_VERSION;
    opt.thread_count = 3;

    if (pzip_create(&opt, &ctx) != PZIP_OK) {
        return 2;
    }
    if (pzip_open_archive(ctx, "pzip_edge_test.zip") != PZIP_OK) {
        pzip_destroy(ctx);
        return 3;
    }

    // 2. Test valid entry name with double dot substring (e.g., "my..data")
    // Previous code would reject this because of find("..")
    pzip_status_t status = pzip_add_path(ctx, testFile, "my..data");
    if (status != PZIP_OK) {
        std::fprintf(stderr, "Failed: 'my..data' was wrongly rejected: %d\n", status);
        pzip_destroy(ctx);
        return 4;
    }

    // 3. Test path traversal: should be rejected
    status = pzip_add_path(ctx, testFile, "../escape");
    if (status == PZIP_OK) {
        std::fprintf(stderr, "Failed: '../escape' was not rejected\n");
        pzip_destroy(ctx);
        return 5;
    }

    status = pzip_add_path(ctx, testFile, "sub/../../escape");
    if (status == PZIP_OK) {
        std::fprintf(stderr, "Failed: 'sub/../../escape' was not rejected\n");
        pzip_destroy(ctx);
        return 6;
    }

    // 4. Test excessively long entry name (>65535 bytes)
    std::string longPrefix(70000, 'a');
    status = pzip_add_path(ctx, testFile, longPrefix.c_str());
    if (status == PZIP_OK) {
        std::fprintf(stderr, "Failed: 70000-byte prefix was not rejected\n");
        pzip_destroy(ctx);
        return 7;
    }

    // 4b. Test non-existent path generates proper last_error
    status = pzip_add_path(ctx, "non_existent_file_12345.txt", "");
    if (status != PZIP_E_NOT_FOUND) {
        std::fprintf(stderr, "Failed: non existent path should return PZIP_E_NOT_FOUND: %d\n", status);
        pzip_destroy(ctx);
        return 11;
    }
    int32_t errCode = 0;
    const char* errMsg = NULL;
    pzip_last_error(ctx, &errCode, &errMsg);
    if (errCode != PZIP_E_NOT_FOUND || errMsg == NULL || std::strstr(errMsg, "path not found") == NULL) {
        std::fprintf(stderr, "Failed: pzip_last_error did not return path not found message\n");
        pzip_destroy(ctx);
        return 12;
    }

    // 4c. Test UTF-8 non-ASCII entry path and progress callback registration
    const char* utf8TestFile = "测试文件_2026.txt";
    const char* utf8Content = "UTF-8 content: 统一字符编码测试\n";
    if (!WriteTestFile(utf8TestFile, utf8Content)) {
        pzip_destroy(ctx);
        return 13;
    }
    status = pzip_add_path(ctx, utf8TestFile, "中文目录");
    if (status != PZIP_OK) {
        std::fprintf(stderr, "Failed: utf-8 path was rejected: %d\n", status);
        pzip_destroy(ctx);
        return 14;
    }

    ProgressRecord compressProg;
    pzip_set_progress_callback(ctx, OnProgress, &compressProg);

    // 5. Test Stateful Codec per-worker thread isolation
    pzip_codec_vtable_t codec;
    std::memset(&codec, 0, sizeof(codec));
    codec.abi_version = PZIP_ABI_VERSION;
    codec.zip_method = 8;
    codec.flags = PZIP_CODEC_FLAG_ZIP_COMPATIBLE;
    codec.create = StatefulCodecCreate;
    codec.destroy = StatefulCodecDestroy;
    codec.compress = StatefulCodecCompress;
    codec.decompress = StatefulCodecDecompress;
    codec.bound = StatefulCodecBound;
    codec.name = StatefulCodecName;

    if (pzip_set_codec(ctx, &codec, NULL) != PZIP_OK) {
        pzip_destroy(ctx);
        return 8;
    }

    // Run pipeline
    status = pzip_run(ctx);
    pzip_destroy(ctx);

    if (status != PZIP_OK) {
        std::fprintf(stderr, "Run failed with status: %d\n", status);
        return 9;
    }

    // Workers should have created contexts and destroyed them
    if (g_createCount.load() == 0 || g_createCount.load() != g_destroyCount.load()) {
        std::fprintf(stderr, "Codec lifecycle mismatch: created=%d, destroyed=%d\n",
                     g_createCount.load(), g_destroyCount.load());
        return 10;
    }

    // 6. Verify compression progress callback reached 100%
    if (compressProg.callCount == 0 || compressProg.lastProcessed != compressProg.totalBytes ||
        compressProg.totalBytes == 0) {
        std::fprintf(stderr, "Compress progress callback failed: calls=%d, last=%llu, total=%llu\n",
                     compressProg.callCount, static_cast<unsigned long long>(compressProg.lastProcessed),
                     static_cast<unsigned long long>(compressProg.totalBytes));
        return 15;
    }

    // 7. Verify Bit 11 (EFS / UTF-8) flag in the generated zip archive
    std::vector<uint8_t> zipBytes;
    if (!ReadWholeFile("pzip_edge_test.zip", &zipBytes) || zipBytes.size() < 30) {
        std::fprintf(stderr, "Failed to read generated zip file\n");
        return 16;
    }
    uint16_t localFlags = static_cast<uint16_t>(zipBytes[6] | (zipBytes[7] << 8));
    if ((localFlags & 0x0800U) == 0U) {
        std::fprintf(stderr, "Bit 11 (UTF-8) not set in local header: 0x%04x\n", localFlags);
        return 17;
    }

    // 8. Verify extraction of archive including UTF-8 path and progress callback
    pzip_ctx_t* extractCtx = NULL;
    pzip_options_t extOpt;
    std::memset(&extOpt, 0, sizeof(extOpt));
    extOpt.abi_version = PZIP_ABI_VERSION;
    extOpt.thread_count = 1;
    if (pzip_create(&extOpt, &extractCtx) != PZIP_OK) {
        return 18;
    }
    ProgressRecord extractProg;
    pzip_set_progress_callback(extractCtx, OnProgress, &extractProg);
    if (pzip_set_codec(extractCtx, &codec, NULL) != PZIP_OK) {
        pzip_destroy(extractCtx);
        return 19;
    }
    status = pzip_extract_archive(extractCtx, "pzip_edge_test.zip", "pzip_edge_out");
    pzip_destroy(extractCtx);
    if (status != PZIP_OK) {
        std::fprintf(stderr, "Extract failed with status: %d\n", status);
        return 20;
    }
    if (extractProg.callCount == 0 || extractProg.lastProcessed != extractProg.totalBytes ||
        extractProg.totalBytes == 0) {
        std::fprintf(stderr, "Extract progress callback failed: calls=%d, last=%llu, total=%llu\n",
                     extractProg.callCount, static_cast<unsigned long long>(extractProg.lastProcessed),
                     static_cast<unsigned long long>(extractProg.totalBytes));
        return 21;
    }

    // Verify extracted UTF-8 file content matches
    std::vector<uint8_t> extractedBytes;
    if (!ReadWholeFile("pzip_edge_out/中文目录/测试文件_2026.txt", &extractedBytes) ||
        extractedBytes.size() != std::strlen(utf8Content) ||
        std::memcmp(extractedBytes.data(), utf8Content, extractedBytes.size()) != 0) {
        std::fprintf(stderr, "Extracted UTF-8 file content mismatch\n");
        return 22;
    }
    std::remove("pzip_edge_out/中文目录/测试文件_2026.txt");
    std::remove("pzip_edge_out/my..data/pzip_edge_test_file.txt");
#if defined(_WIN32)
    _rmdir("pzip_edge_out/my..data");
    _rmdir("pzip_edge_out/中文目录");
    _rmdir("pzip_edge_out");
#else
    rmdir("pzip_edge_out/my..data");
    rmdir("pzip_edge_out/中文目录");
    rmdir("pzip_edge_out");
#endif

    // 9. Verify Data Descriptor (Bit 3) extraction support (with and without signature)
    if (!TestDataDescriptorExtract(true)) {
        std::fprintf(stderr, "TestDataDescriptorExtract(with signature) failed\n");
        return 23;
    }
    if (!TestDataDescriptorExtract(false)) {
        std::fprintf(stderr, "TestDataDescriptorExtract(without signature) failed\n");
        return 24;
    }

    std::remove(testFile);
    std::remove(utf8TestFile);
    std::remove("pzip_edge_test.zip");
    return 0;
}
