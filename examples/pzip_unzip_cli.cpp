#include <stdint.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

#include "pzip.h"

struct XorEncryptionCtx {
    std::string m_password;
};

static void* CreateXorEncryption(void* user) {
    const char* password = reinterpret_cast<const char*>(user);
    XorEncryptionCtx* ctx = new (std::nothrow) XorEncryptionCtx();
    if (ctx == NULL) {
        return NULL;
    }
    ctx->m_password = password == NULL ? "" : password;
    if (ctx->m_password.empty()) {
        delete ctx;
        return NULL;
    }
    return ctx;
}

static void DestroyXorEncryption(void* encryptionCtx) {
    XorEncryptionCtx* ctx = reinterpret_cast<XorEncryptionCtx*>(encryptionCtx);
    delete ctx;
}

static pzip_status_t XorTransform(void* encryptionCtx, const uint8_t* src, size_t srcSize,
                                  uint8_t* dst, size_t dstCap, size_t* dstSize) {
    if (encryptionCtx == NULL || dstSize == NULL || (srcSize > 0 && (src == NULL || dst == NULL))) {
        return PZIP_E_INVALID_ARG;
    }
    if (dstCap < srcSize) {
        return PZIP_E_NO_MEMORY;
    }

    XorEncryptionCtx* ctx = reinterpret_cast<XorEncryptionCtx*>(encryptionCtx);
    const size_t keySize = ctx->m_password.size();
    if (keySize == 0) {
        return PZIP_E_INVALID_ARG;
    }

    for (size_t i = 0; i < srcSize; ++i) {
        dst[i] = static_cast<uint8_t>(src[i] ^ static_cast<uint8_t>(ctx->m_password[i % keySize]));
    }
    *dstSize = srcSize;
    return PZIP_OK;
}

static size_t XorBound(void* encryptionCtx, size_t srcSize) {
    (void)encryptionCtx;
    return srcSize;
}

static const char* XorName(void* encryptionCtx) {
    (void)encryptionCtx;
    return "xor-password";
}

static bool IsSupportedCodecName(const std::string& codecName) {
    return codecName == "zlib" || codecName == "zstd" || codecName == "lz4";
}

static void PrintUsage(const char* argv0) {
    std::fprintf(stderr,
                 "Usage: %s [options] <archive.zip> <output_dir>\n"
                 "Options:\n"
                 "  --codec <name>     Decompression codec: zlib, zstd, or lz4 (default: zstd)\n"
                 "  -p <password>      Set password for decryption\n",
                 argv0);
}

int main(int argc, char** argv) {
    pzip_ctx_t* ctx = NULL;
    pzip_options_t opt;
    pzip_codec_vtable_t codec;
    pzip_encryption_vtable_t encryption;
    std::string codecName = "zstd";
    std::string password;
    std::string archivePath;
    std::string outputDir;
    int32_t errCode = 0;
    const char* errMsg = NULL;

    std::memset(&opt, 0, sizeof(opt));
    opt.abi_version = PZIP_ABI_VERSION;
    opt.thread_count = 0;
    opt.chunk_size_kb = 256;
    opt.enable_solid_mode = 0;
    opt.max_file_count = 1000000U;
    opt.max_total_input_bytes = 1ULL << 40;

    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--codec" && i + 1 < argc) {
            codecName = argv[++i];
        } else if (arg == "-p" && i + 1 < argc) {
            password = argv[++i];
        } else if (!arg.empty() && arg[0] == '-') {
            PrintUsage(argc > 0 ? argv[0] : "pzip-unzip");
            return 2;
        } else if (archivePath.empty()) {
            archivePath = arg;
        } else if (outputDir.empty()) {
            outputDir = arg;
        } else {
            PrintUsage(argc > 0 ? argv[0] : "pzip-unzip");
            return 2;
        }
    }

    if (archivePath.empty() || outputDir.empty() || !IsSupportedCodecName(codecName)) {
        PrintUsage(argc > 0 ? argv[0] : "pzip-unzip");
        return 2;
    }

    if (pzip_create(&opt, &ctx) != PZIP_OK) {
        std::fprintf(stderr, "pzip_create failed\n");
        return 1;
    }

    pzip_status_t codecStatus = PZIP_E_INVALID_ARG;
    if (codecName == "zlib") {
        codecStatus = pzip_make_default_zlib_codec(&codec);
    } else if (codecName == "zstd") {
        codecStatus = pzip_make_default_zstd_codec(&codec);
    } else if (codecName == "lz4") {
        codecStatus = pzip_make_default_lz4_codec(&codec);
    }
    if (codecStatus != PZIP_OK || pzip_set_codec(ctx, &codec, NULL) != PZIP_OK) {
        std::fprintf(stderr, "failed to configure codec '%s'\n", codecName.c_str());
        pzip_destroy(ctx);
        return 1;
    }

    if (!password.empty()) {
        std::memset(&encryption, 0, sizeof(encryption));
        encryption.abi_version = PZIP_ABI_VERSION;
        encryption.algorithm_id = 1;
        encryption.flags = PZIP_ENCRYPTION_FLAG_STREAM;
        encryption.create = &CreateXorEncryption;
        encryption.destroy = &DestroyXorEncryption;
        encryption.encrypt = &XorTransform;
        encryption.decrypt = &XorTransform;
        encryption.bound = &XorBound;
        encryption.name = &XorName;
        if (pzip_set_encryption(ctx, &encryption, const_cast<char*>(password.c_str())) != PZIP_OK ||
            pzip_set_encryption_enabled(ctx, 1) != PZIP_OK) {
            std::fprintf(stderr, "failed to enable decryption\n");
            pzip_destroy(ctx);
            return 1;
        }
    }

    if (pzip_extract_archive(ctx, archivePath.c_str(), outputDir.c_str()) != PZIP_OK) {
        if (pzip_last_error(ctx, &errCode, &errMsg) == PZIP_OK) {
            std::fprintf(stderr, "unzip failed: code=%d msg=%s\n", errCode, errMsg != NULL ? errMsg : "<null>");
        } else {
            std::fprintf(stderr, "unzip failed\n");
        }
        pzip_destroy(ctx);
        return 1;
    }

    pzip_destroy(ctx);
    return 0;
}
