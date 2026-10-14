#include <inttypes.h>
#include <stdint.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "ghc/filesystem.hpp"
#include "pzip.h"

namespace fs = ghc::filesystem;

static bool IsSupportedCodecName(const std::string& codecName) {
    return codecName == "zlib" || codecName == "zstd" || codecName == "lz4";
}

static void PrintUsage(const char* argv0) {
    std::fprintf(stderr,
                 "Usage:\n"
                 "  Create:  %s [options] <archive.zip> <input1> [input2 ...]\n"
                 "  Append:  %s [options] -a <archive.zip> <input1> [input2 ...]\n"
                 "  Delete:  %s [options] -d <entry> [-d ...] <archive.zip>\n"
                 "  Rename:  %s [options] --rename <old> <new> <archive.zip>\n"
                 "  Modify:  %s [options] [-a] [-d <entry>] [--rename <old> <new>] <archive.zip> [inputs...]\n"
                 "\n"
                 "Options:\n"
                 "  -a, --append             Append to existing archive (creates if not existing)\n"
                 "  -d, --delete <entry>     Delete specified entry from archive (use with -r for dir)\n"
                 "  -r                       Recursively add directory inputs / recursive delete\n"
                 "  --rename <old> <new>     Rename entry inside archive\n"
                 "  -p <password>            Enable password encryption\n"
                 "  --crypto <mode>          Crypto mode: aes256 (default), zipcrypto, or aes128\n"
                 "  -l <0-9>                 zlib compression level (default: 6)\n"
                 "  -t <threads>             Worker thread count (0 = auto)\n"
                 "  --codec <name>           Compression codec: zlib, zstd, or lz4 (default: zstd)\n"
                 "  --prefix <name>          Entry prefix for all inputs\n",
                 argv0, argv0, argv0, argv0, argv0);
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
    std::vector<std::string> inputs;
    std::vector<std::string> deleteEntries;
    std::vector<std::pair<std::string, std::string>> renamePairs;
    std::string archivePath;
    std::string entryPrefix;
    std::string codecName = "zstd";
    std::string cryptoName = "aes256";
    std::string password;
    bool recursive = false;
    bool appendRequested = false;
    int32_t compressionLevel = 6;
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

    for (int32_t i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "-r") {
            recursive = true;
        } else if (arg == "-a" || arg == "--append") {
            appendRequested = true;
        } else if ((arg == "-d" || arg == "--delete") && i + 1 < argc) {
            deleteEntries.push_back(argv[++i]);
        } else if (arg == "--rename" && i + 2 < argc) {
            renamePairs.emplace_back(argv[i + 1], argv[i + 2]);
            i += 2;
        } else if (arg == "-p" && i + 1 < argc) {
            password = argv[++i];
        } else if (arg == "--crypto" && i + 1 < argc) {
            cryptoName = argv[++i];
        } else if (arg == "-l" && i + 1 < argc) {
            compressionLevel = static_cast<int32_t>(std::atoi(argv[++i]));
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

    pzip_crypto_mode_t cryptoMode = PZIP_CRYPTO_AES256;
    if (cryptoName == "aes256") {
        cryptoMode = PZIP_CRYPTO_AES256;
    } else if (cryptoName == "zipcrypto") {
        cryptoMode = PZIP_CRYPTO_ZIPCRYPTO;
    } else if (cryptoName == "aes128") {
        cryptoMode = PZIP_CRYPTO_AES128;
    } else {
        std::fprintf(stderr, "unsupported --crypto mode '%s', expected aes256, zipcrypto, or aes128\n",
                     cryptoName.c_str());
        return 2;
    }

    pzip_open_mode_t openMode = PZIP_MODE_CREATE;
    if (!deleteEntries.empty() || !renamePairs.empty()) {
        openMode = PZIP_MODE_MODIFY;
    } else if (appendRequested) {
        openMode = PZIP_MODE_APPEND;
    }

    if (archivePath.empty() || compressionLevel < 0 || compressionLevel > 9 ||
        !IsSupportedCodecName(codecName)) {
        PrintUsage(argc > 0 ? argv[0] : "pzip-zip");
        return 2;
    }

    if (openMode == PZIP_MODE_CREATE && inputs.empty()) {
        PrintUsage(argc > 0 ? argv[0] : "pzip-zip");
        return 2;
    }

    if (openMode == PZIP_MODE_APPEND && inputs.empty()) {
        PrintUsage(argc > 0 ? argv[0] : "pzip-zip");
        return 2;
    }

    if (openMode == PZIP_MODE_MODIFY && inputs.empty() && deleteEntries.empty() && renamePairs.empty()) {
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
        if (pzip_set_password(ctx, password.c_str()) != PZIP_OK ||
            pzip_set_crypto_mode(ctx, cryptoMode) != PZIP_OK) {
            std::fprintf(stderr, "failed to configure password encryption\n");
            pzip_destroy(ctx);
            return 1;
        }
    }

    if (pzip_open_archive_mode(ctx, archivePath.c_str(), openMode) != PZIP_OK) {
        if (pzip_last_error(ctx, &errCode, &errMsg) == PZIP_OK) {
            std::fprintf(stderr, "failed to open archive '%s': code=%" PRId32 " msg=%s\n",
                         archivePath.c_str(), errCode, errMsg != NULL ? errMsg : "<null>");
        } else {
            std::fprintf(stderr, "failed to open archive '%s'\n", archivePath.c_str());
        }
        pzip_destroy(ctx);
        return 1;
    }

    for (size_t i = 0; i < deleteEntries.size(); ++i) {
        const int32_t recFlag = recursive ? 1 : 0;
        if (pzip_remove_entry(ctx, deleteEntries[i].c_str(), recFlag) != PZIP_OK) {
            if (pzip_last_error(ctx, &errCode, &errMsg) == PZIP_OK) {
                std::fprintf(stderr, "failed to mark entry '%s' for deletion: code=%" PRId32 " msg=%s\n",
                             deleteEntries[i].c_str(), errCode, errMsg != NULL ? errMsg : "<null>");
            } else {
                std::fprintf(stderr, "failed to mark entry '%s' for deletion\n", deleteEntries[i].c_str());
            }
            pzip_destroy(ctx);
            return 1;
        }
    }

    for (size_t i = 0; i < renamePairs.size(); ++i) {
        if (pzip_rename_entry(ctx, renamePairs[i].first.c_str(), renamePairs[i].second.c_str()) != PZIP_OK) {
            if (pzip_last_error(ctx, &errCode, &errMsg) == PZIP_OK) {
                std::fprintf(stderr, "failed to rename entry '%s' -> '%s': code=%" PRId32 " msg=%s\n",
                             renamePairs[i].first.c_str(), renamePairs[i].second.c_str(),
                             errCode, errMsg != NULL ? errMsg : "<null>");
            } else {
                std::fprintf(stderr, "failed to rename entry '%s'\n", renamePairs[i].first.c_str());
            }
            pzip_destroy(ctx);
            return 1;
        }
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
                std::fprintf(stderr, "failed to add input '%s': code=%" PRId32 " msg=%s\n", inputs[i].c_str(),
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
            std::fprintf(stderr, "zip failed: code=%" PRId32 " msg=%s\n", errCode,
                         errMsg != NULL ? errMsg : "<null>");
        } else {
            std::fprintf(stderr, "zip failed\n");
        }
        pzip_destroy(ctx);
        return 1;
    }

    pzip_destroy(ctx);
    return 0;
}
