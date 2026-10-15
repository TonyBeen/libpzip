#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <direct.h>
#define PZIP_TEST_RMDIR(p) _rmdir(p)
#else
#include <unistd.h>
#define PZIP_TEST_RMDIR(p) rmdir(p)
#endif

#include "pzip.h"

namespace {

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

}  // namespace

int main() {
    const char* sampleFile = "test_pass_sample.txt";
    const char* sampleContent = "Hello standard ZIP password encryption! 这是一个跨平台加密测试内容。\n";
    if (!WriteTestFile(sampleFile, sampleContent)) {
        std::fprintf(stderr, "Failed to write sample test file\n");
        return 1;
    }

    // =========================================================================
    // Test 1: WinZip AES-256 (Default Standard Encryption)
    // =========================================================================
    {
        const char* zipName = "test_aes256.zip";
        const char* outDir = "test_aes256_out";
        const char* password = "MyAesPassword2026!";

        pzip_ctx_t* ctx = NULL;
        pzip_options_t opt;
        std::memset(&opt, 0, sizeof(opt));
        opt.abi_version = PZIP_ABI_VERSION;
        opt.thread_count = 2;

        assert(pzip_create(&opt, &ctx) == PZIP_OK);
        assert(pzip_open_archive(ctx, zipName) == PZIP_OK);
        // By default, setting password enables PZIP_CRYPTO_AES256
        assert(pzip_set_password(ctx, password) == PZIP_OK);
        assert(pzip_add_path(ctx, sampleFile, "secure_folder") == PZIP_OK);
        assert(pzip_run(ctx) == PZIP_OK);
        pzip_destroy(ctx);

        // Binary check: verify Method 99, Bit 0 set, Extra Field 0x9901 present
        std::vector<uint8_t> zipBytes;
        assert(ReadWholeFile(zipName, &zipBytes));
        assert(zipBytes.size() > 30);
        uint16_t localFlags = static_cast<uint16_t>(zipBytes[6] | (zipBytes[7] << 8));
        uint16_t localMethod = static_cast<uint16_t>(zipBytes[8] | (zipBytes[9] << 8));
        assert((localFlags & 0x0001U) != 0);  // Bit 0: Encrypted
        assert(localMethod == 99);            // Method 99 for WinZip AES

        // Verify Extra Field 0x9901
        uint16_t nameLen = static_cast<uint16_t>(zipBytes[26] | (zipBytes[27] << 8));
        uint16_t extraLen = static_cast<uint16_t>(zipBytes[28] | (zipBytes[29] << 8));
        assert(extraLen >= 11);
        size_t extraOffset = 30 + nameLen;
        uint16_t extraHeaderId = static_cast<uint16_t>(zipBytes[extraOffset] | (zipBytes[extraOffset + 1] << 8));
        assert(extraHeaderId == 0x9901U);
        uint8_t strength = zipBytes[extraOffset + 8];
        assert(strength == 3); // 3 = AES-256

        // 1a. Extract without password -> should fail with PZIP_E_INVALID_ARG
        pzip_ctx_t* noPassCtx = NULL;
        assert(pzip_create(&opt, &noPassCtx) == PZIP_OK);
        pzip_status_t status = pzip_extract_archive(noPassCtx, zipName, outDir);
        assert(status == PZIP_E_INVALID_ARG);
        pzip_destroy(noPassCtx);

        // 1b. Extract with wrong password -> should fail with PZIP_E_CODEC
        pzip_ctx_t* wrongPassCtx = NULL;
        assert(pzip_create(&opt, &wrongPassCtx) == PZIP_OK);
        assert(pzip_set_password(wrongPassCtx, "WrongPassword!") == PZIP_OK);
        status = pzip_extract_archive(wrongPassCtx, zipName, outDir);
        assert(status == PZIP_E_CODEC);
        pzip_destroy(wrongPassCtx);

        // 1c. Extract with correct password -> should succeed and match content
        pzip_ctx_t* correctPassCtx = NULL;
        assert(pzip_create(&opt, &correctPassCtx) == PZIP_OK);
        assert(pzip_set_password(correctPassCtx, password) == PZIP_OK);
        status = pzip_extract_archive(correctPassCtx, zipName, outDir);
        assert(status == PZIP_OK);
        pzip_destroy(correctPassCtx);

        std::string extractedFile = std::string(outDir) + "/secure_folder/" + sampleFile;
        std::vector<uint8_t> outBytes;
        assert(ReadWholeFile(extractedFile.c_str(), &outBytes));
        assert(outBytes.size() == std::strlen(sampleContent));
        assert(std::memcmp(outBytes.data(), sampleContent, outBytes.size()) == 0);

        // Cleanup
        std::remove(extractedFile.c_str());
        std::string folder = std::string(outDir) + "/secure_folder";
        PZIP_TEST_RMDIR(folder.c_str());
        PZIP_TEST_RMDIR(outDir);
        std::remove(zipName);
    }

    // =========================================================================
    // Test 2: Traditional PKWARE ZipCrypto
    // =========================================================================
    {
        const char* zipName = "test_zipcrypto.zip";
        const char* outDir = "test_zipcrypto_out";
        const char* password = "MyZipCryptoPass123";

        pzip_ctx_t* ctx = NULL;
        pzip_options_t opt;
        std::memset(&opt, 0, sizeof(opt));
        opt.abi_version = PZIP_ABI_VERSION;
        opt.thread_count = 2;

        assert(pzip_create(&opt, &ctx) == PZIP_OK);
        assert(pzip_open_archive(ctx, zipName) == PZIP_OK);
        assert(pzip_set_password(ctx, password) == PZIP_OK);
        assert(pzip_set_crypto_mode(ctx, PZIP_CRYPTO_ZIPCRYPTO) == PZIP_OK);
        assert(pzip_add_path(ctx, sampleFile, "zipcrypto_folder") == PZIP_OK);
        assert(pzip_run(ctx) == PZIP_OK);
        pzip_destroy(ctx);

        // Binary check: verify Bit 0 set, method is NOT 99 (standard Deflate method 8)
        std::vector<uint8_t> zipBytes;
        assert(ReadWholeFile(zipName, &zipBytes));
        assert(zipBytes.size() > 30);
        uint16_t localFlags = static_cast<uint16_t>(zipBytes[6] | (zipBytes[7] << 8));
        uint16_t localMethod = static_cast<uint16_t>(zipBytes[8] | (zipBytes[9] << 8));
        assert((localFlags & 0x0001U) != 0);  // Bit 0: Encrypted
        assert(localMethod != 99);

        // 2a. Interoperability check with system /usr/bin/unzip tool (if available)
        char unzipCmd[512];
        std::snprintf(unzipCmd, sizeof(unzipCmd), "unzip -P %s -t %s > /dev/null 2>&1", password, zipName);
        int ret = std::system(unzipCmd);
        if (ret == 0) {
            std::printf("Verified with system /usr/bin/unzip: test passed!\n");
        }

        // 2b. Extract with libpzip using wrong password -> should fail
        pzip_ctx_t* wrongCtx = NULL;
        assert(pzip_create(&opt, &wrongCtx) == PZIP_OK);
        assert(pzip_set_password(wrongCtx, "IncorrectPass!") == PZIP_OK);
        pzip_status_t status = pzip_extract_archive(wrongCtx, zipName, outDir);
        assert(status == PZIP_E_CODEC);
        pzip_destroy(wrongCtx);

        // 2c. Extract with libpzip using correct password -> should succeed
        pzip_ctx_t* correctCtx = NULL;
        assert(pzip_create(&opt, &correctCtx) == PZIP_OK);
        assert(pzip_set_password(correctCtx, password) == PZIP_OK);
        status = pzip_extract_archive(correctCtx, zipName, outDir);
        assert(status == PZIP_OK);
        pzip_destroy(correctCtx);

        std::string extractedFile = std::string(outDir) + "/zipcrypto_folder/" + sampleFile;
        std::vector<uint8_t> outBytes;
        assert(ReadWholeFile(extractedFile.c_str(), &outBytes));
        assert(outBytes.size() == std::strlen(sampleContent));
        assert(std::memcmp(outBytes.data(), sampleContent, outBytes.size()) == 0);

        // Cleanup
        std::remove(extractedFile.c_str());
        std::string folder = std::string(outDir) + "/zipcrypto_folder";
        PZIP_TEST_RMDIR(folder.c_str());
        PZIP_TEST_RMDIR(outDir);
        std::remove(zipName);
    }

    // =========================================================================
    // Test 3: WinZip AES-128
    // =========================================================================
    {
        const char* zipName = "test_aes128.zip";
        const char* outDir = "test_aes128_out";
        const char* password = "MyAes128Password!";

        pzip_ctx_t* ctx = NULL;
        pzip_options_t opt;
        std::memset(&opt, 0, sizeof(opt));
        opt.abi_version = PZIP_ABI_VERSION;
        opt.thread_count = 1;

        assert(pzip_create(&opt, &ctx) == PZIP_OK);
        assert(pzip_open_archive(ctx, zipName) == PZIP_OK);
        assert(pzip_set_password(ctx, password) == PZIP_OK);
        assert(pzip_set_crypto_mode(ctx, PZIP_CRYPTO_AES128) == PZIP_OK);
        assert(pzip_add_path(ctx, sampleFile, "aes128_folder") == PZIP_OK);
        assert(pzip_run(ctx) == PZIP_OK);
        pzip_destroy(ctx);

        pzip_ctx_t* extCtx = NULL;
        assert(pzip_create(&opt, &extCtx) == PZIP_OK);
        assert(pzip_set_password(extCtx, password) == PZIP_OK);
        assert(pzip_extract_archive(extCtx, zipName, outDir) == PZIP_OK);
        pzip_destroy(extCtx);

        std::string extractedFile = std::string(outDir) + "/aes128_folder/" + sampleFile;
        std::vector<uint8_t> outBytes;
        assert(ReadWholeFile(extractedFile.c_str(), &outBytes));
        assert(outBytes.size() == std::strlen(sampleContent));
        assert(std::memcmp(outBytes.data(), sampleContent, outBytes.size()) == 0);

        // Cleanup
        std::remove(extractedFile.c_str());
        std::string folder = std::string(outDir) + "/aes128_folder";
        PZIP_TEST_RMDIR(folder.c_str());
        PZIP_TEST_RMDIR(outDir);
        std::remove(zipName);
    }

    std::remove(sampleFile);
    std::printf("All standard password encryption tests passed successfully!\n");
    return 0;
}
