#include <stdint.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "ghc/filesystem.hpp"
#include "pzip.h"

namespace fs = ghc::filesystem;

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
                 "Usage: %s [options] <archive.zip> <input1> [input2 ...]\n"
                 "Options:\n"
                 "  -r                 Recursively add directory inputs\n"
                 "  -p <password>      Enable password encryption\n"
                 "  -l <0-9>           zlib compression level (default: 6)\n"
                 "  -t <threads>       Worker thread count (0 = auto)\n"
                 "  --codec <name>     Compression codec: zlib, zstd, or lz4 (default: zlib)\n"
                 "  --prefix <name>    Entry prefix for all inputs\n",
                 argv0);
}

static bool AddNonRecursivePath(pzip_ctx_t* ctx, const fs::path& input, const std::string& prefix) {
    if (fs::is_regular_file(input)) {
        return pzip_add_path(ctx, input.string().c_str(), prefix.c_str()) == PZIP_OK;
    }
    if (!fs::is_directory(input)) {
        return false;
    }

    for (fs::directory_iterator it(input); it != fs::directory_iterator(); ++it) {
        if (!fs::is_regular_file(it->path())) {
            continue;
        }
        if (pzip_add_path(ctx, it->path().string().c_str(), prefix.c_str()) != PZIP_OK) {
            return false;
        }
    }
    return true;
}

int main(int argc, char** argv) {
    pzip_ctx_t* ctx = NULL;
    pzip_options_t opt;
    pzip_codec_vtable_t codec;
    pzip_encryption_vtable_t encryption;
    std::vector<std::string> inputs;
    std::string archivePath;
    std::string entryPrefix;
    std::string codecName = "zlib";
    std::string password;
    bool recursive = false;
    int compressionLevel = 6;
    bool compressionLevelExplicit = false;
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
        if (arg == "-r") {
            recursive = true;
        } else if (arg == "-p" && i + 1 < argc) {
            password = argv[++i];
        } else if (arg == "-l" && i + 1 < argc) {
            compressionLevel = std::atoi(argv[++i]);
            compressionLevelExplicit = true;
        } else if (arg == "-t" && i + 1 < argc) {
            opt.thread_count = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (arg == "--codec" && i + 1 < argc) {
            codecName = argv[++i];
        } else if (arg == "--prefix" && i + 1 < argc) {
            entryPrefix = argv[++i];
        } else if (!arg.empty() && arg[0] == '-') {
            PrintUsage(argc > 0 ? argv[0] : "pzip-zip");
            return 2;
        } else if (archivePath.empty()) {
            archivePath = arg;
        } else {
            inputs.push_back(arg);
        }
    }

    if (archivePath.empty() || inputs.empty() || compressionLevel < 0 || compressionLevel > 9 ||
        !IsSupportedCodecName(codecName)) {
        PrintUsage(argc > 0 ? argv[0] : "pzip-zip");
        return 2;
    }

    if (codecName != "zlib" && compressionLevelExplicit) {
        std::fprintf(stderr, "-l is only supported with --codec zlib\n");
        return 2;
    }

    if (pzip_create(&opt, &ctx) != PZIP_OK) {
        std::fprintf(stderr, "pzip_create failed\n");
        return 1;
    }

    pzip_status_t codecStatus = PZIP_E_INVALID_ARG;
    if (codecName == "zlib") {
        codecStatus = pzip_make_default_zlib_codec(&codec);
        if (codecStatus == PZIP_OK) {
            codecStatus = pzip_set_codec(ctx, &codec, &compressionLevel);
        }
    } else if (codecName == "zstd") {
        codecStatus = pzip_make_default_zstd_codec(&codec);
        if (codecStatus == PZIP_OK) {
            codecStatus = pzip_set_codec(ctx, &codec, NULL);
        }
    } else if (codecName == "lz4") {
        codecStatus = pzip_make_default_lz4_codec(&codec);
        if (codecStatus == PZIP_OK) {
            codecStatus = pzip_set_codec(ctx, &codec, NULL);
        }
    }
    if (codecStatus != PZIP_OK) {
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
            std::fprintf(stderr, "failed to enable encryption\n");
            pzip_destroy(ctx);
            return 1;
        }
    }

    if (pzip_open_archive(ctx, archivePath.c_str()) != PZIP_OK) {
        std::fprintf(stderr, "failed to open output archive\n");
        pzip_destroy(ctx);
        return 1;
    }

    for (size_t i = 0; i < inputs.size(); ++i) {
        const fs::path inputPath(inputs[i]);
        bool ok = false;
        if (recursive) {
            ok = pzip_add_path(ctx, inputPath.string().c_str(), entryPrefix.c_str()) == PZIP_OK;
        } else {
            ok = AddNonRecursivePath(ctx, inputPath, entryPrefix);
        }
        if (!ok) {
            if (pzip_last_error(ctx, &errCode, &errMsg) == PZIP_OK) {
                std::fprintf(stderr, "failed to add input '%s': code=%d msg=%s\n", inputs[i].c_str(),
                             errCode, errMsg != NULL ? errMsg : "<null>");
            } else {
                std::fprintf(stderr, "failed to add input '%s'\n", inputs[i].c_str());
            }
            pzip_destroy(ctx);
            return 1;
        }
    }

    if (pzip_run(ctx) != PZIP_OK) {
        if (pzip_last_error(ctx, &errCode, &errMsg) == PZIP_OK) {
            std::fprintf(stderr, "zip failed: code=%d msg=%s\n", errCode, errMsg != NULL ? errMsg : "<null>");
        } else {
            std::fprintf(stderr, "zip failed\n");
        }
        pzip_destroy(ctx);
        return 1;
    }

    pzip_destroy(ctx);
    return 0;
}
