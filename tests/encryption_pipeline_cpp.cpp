#include <errno.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#include <direct.h>
#define PZIP_TEST_MKDIR(path) _mkdir(path)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define PZIP_TEST_MKDIR(path) mkdir(path, 0755)
#endif

#include "pzip.h"

namespace {

size_t g_encryptCalls = 0;

bool EnsureDir(const char* path) {
    if (PZIP_TEST_MKDIR(path) == 0) {
        return true;
    }
    return errno == EEXIST;
}

bool WritePatternFile(const char* path, const char* seed, size_t repeatCount) {
    FILE* out = std::fopen(path, "wb");
    const size_t seedLen = std::strlen(seed);
    if (out == NULL) {
        return false;
    }
    for (size_t i = 0; i < repeatCount; ++i) {
        if (std::fwrite(seed, 1, seedLen, out) != seedLen) {
            std::fclose(out);
            return false;
        }
    }
    return std::fclose(out) == 0;
}

void* CreateEncryption(void*) {
    return reinterpret_cast<void*>(0x1);
}

void DestroyEncryption(void*) {}

pzip_status_t EncryptPassthrough(void*, const uint8_t* src, size_t srcSize, uint8_t* dst,
                                 size_t dstCap, size_t* dstSize) {
    ++g_encryptCalls;
    if (dstSize == NULL) {
        return PZIP_E_INVALID_ARG;
    }
    if (dstCap < srcSize) {
        return PZIP_E_NO_MEMORY;
    }
    if (srcSize > 0 && src != NULL && dst != NULL) {
        std::memcpy(dst, src, srcSize);
    }
    *dstSize = srcSize;
    return PZIP_OK;
}

size_t EncryptBound(void*, size_t srcSize) {
    return srcSize;
}

const char* EncryptName(void*) {
    return "passthrough-encrypt";
}

}  // namespace

int main() {
    const char* rootDir = "pzip_encrypt_case";
    const char* inputDir = "pzip_encrypt_case/input";
    const char* archivePath = "pzip_encrypt_case/out.zip";
    pzip_ctx_t* ctx = NULL;
    pzip_options_t opt;
    pzip_encryption_vtable_t encryption;
    pzip_status_t status;

    if (!EnsureDir(rootDir) || !EnsureDir(inputDir)) {
        return 70;
    }
    if (!WritePatternFile("pzip_encrypt_case/input/a.txt", "encrypt-hello\n", 4096)) {
        return 71;
    }

    std::memset(&opt, 0, sizeof(opt));
    opt.abi_version = PZIP_ABI_VERSION;
    opt.thread_count = 2;
    opt.chunk_size_kb = 4;
    opt.enable_solid_mode = 0;
    opt.max_file_count = 8;
    opt.max_total_input_bytes = 1ULL << 25;

    if (pzip_create(&opt, &ctx) != PZIP_OK) {
        return 72;
    }

    std::memset(&encryption, 0, sizeof(encryption));
    encryption.abi_version = PZIP_ABI_VERSION;
    encryption.algorithm_id = 1;
    encryption.flags = PZIP_ENCRYPTION_FLAG_STREAM;
    encryption.create = &CreateEncryption;
    encryption.destroy = &DestroyEncryption;
    encryption.encrypt = &EncryptPassthrough;
    encryption.bound = &EncryptBound;
    encryption.name = &EncryptName;

    if (pzip_set_encryption(ctx, &encryption, NULL) != PZIP_OK) {
        pzip_destroy(ctx);
        return 73;
    }
    if (pzip_set_encryption_enabled(ctx, 1) != PZIP_OK) {
        pzip_destroy(ctx);
        return 74;
    }
    if (pzip_open_archive(ctx, archivePath) != PZIP_OK) {
        pzip_destroy(ctx);
        return 75;
    }
    if (pzip_add_path(ctx, inputDir, "enc") != PZIP_OK) {
        pzip_destroy(ctx);
        return 76;
    }

    status = pzip_run(ctx);
    pzip_destroy(ctx);

    if (status != PZIP_OK) {
        return 77;
    }
    if (g_encryptCalls == 0) {
        return 78;
    }
    return 0;
}
