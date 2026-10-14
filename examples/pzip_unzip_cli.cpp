#include <inttypes.h>
#include <stdint.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

#include "pzip.h"

static bool IsSupportedCodecName(const std::string& codecName) {
    return codecName == "zlib" || codecName == "zstd" || codecName == "lz4";
}

static const char* GetMethodName(uint16_t method) {
    switch (method) {
        case 0:
            return "Store";
        case 8:
            return "Deflate";
        case 93:
            return "Zstd";
        case 99:
            return "AES-Wrap";
        case 0x4f4cU:
            return "LZ4";
        default:
            return "Unknown";
    }
}

struct ListSummary {
    uint64_t totalUncompressed;
    uint64_t totalCompressed;
    uint32_t fileCount;
    uint32_t dirCount;
};

static int32_t ListEntryCallback(const pzip_entry_info_t* entry, void* userData) {
    if (entry == NULL || userData == NULL) {
        return 1;
    }
    ListSummary* summary = static_cast<ListSummary*>(userData);
    summary->totalUncompressed += entry->uncompressed_size;
    summary->totalCompressed += entry->compressed_size;
    if (entry->is_directory != 0) {
        summary->dirCount++;
    } else {
        summary->fileCount++;
    }

    std::printf("%10" PRIu64 "  %10" PRIu64 "  %-8s  %-9s  %s\n",
                entry->uncompressed_size,
                entry->compressed_size,
                GetMethodName(entry->method),
                (entry->is_encrypted != 0) ? "Yes" : "No",
                entry->entry_name != NULL ? entry->entry_name : "");
    return 0;
}

static void PrintUsage(const char* argv0) {
    std::fprintf(stderr,
                 "Usage:\n"
                 "  Extract all:    %s [options] <archive.zip> <output_dir>\n"
                 "  Extract single: %s [options] -e <entry> <archive.zip> <output_file>\n"
                 "  List entries:   %s -l <archive.zip>\n"
                 "\n"
                 "Options:\n"
                 "  -l, --list               List contents of archive without extracting\n"
                 "  -e, --entry <entry>      Extract a single entry to target output file\n"
                 "  -p <password>            Set password for decryption\n"
                 "  --codec <name>           Decompression codec: zlib, zstd, or lz4 (default: zstd)\n",
                 argv0, argv0, argv0);
}

int main(int argc, char** argv) {
    pzip_ctx_t* ctx = NULL;
    pzip_options_t opt;
    pzip_codec_vtable_t codec;
    std::string codecName = "zstd";
    std::string password;
    std::string archivePath;
    std::string outputTarget;
    std::string singleEntry;
    bool listMode = false;
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
        if (arg == "-l" || arg == "--list") {
            listMode = true;
        } else if ((arg == "-e" || arg == "--entry") && i + 1 < argc) {
            singleEntry = argv[++i];
        } else if (arg == "--codec" && i + 1 < argc) {
            codecName = argv[++i];
        } else if (arg == "-p" && i + 1 < argc) {
            password = argv[++i];
        } else if (!arg.empty() && arg[0] == '-') {
            PrintUsage(argc > 0 ? argv[0] : "pzip-unzip");
            return 2;
        } else if (archivePath.empty()) {
            archivePath = arg;
        } else if (outputTarget.empty()) {
            outputTarget = arg;
        } else {
            PrintUsage(argc > 0 ? argv[0] : "pzip-unzip");
            return 2;
        }
    }

    if (listMode) {
        if (archivePath.empty()) {
            PrintUsage(argc > 0 ? argv[0] : "pzip-unzip");
            return 2;
        }
        ListSummary summary;
        std::memset(&summary, 0, sizeof(summary));

        std::printf("Archive:  %s\n", archivePath.c_str());
        std::printf("    Length  Compressed  Method    Encrypted  Name\n");
        std::printf("----------  ----------  --------  ---------  ------------------------\n");
        const pzip_status_t listSt = pzip_list_archive(archivePath.c_str(), ListEntryCallback, &summary);
        if (listSt != PZIP_OK) {
            std::fprintf(stderr, "failed to list archive '%s': status=%" PRId32 "\n",
                         archivePath.c_str(), static_cast<int32_t>(listSt));
            return 1;
        }
        std::printf("----------  ----------  --------  ---------  ------------------------\n");
        std::printf("%10" PRIu64 "  %10" PRIu64 "                       %" PRIu32 " file(s), %" PRIu32 " dir(s)\n",
                    summary.totalUncompressed, summary.totalCompressed, summary.fileCount, summary.dirCount);
        return 0;
    }

    if (archivePath.empty() || outputTarget.empty() || !IsSupportedCodecName(codecName)) {
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
        if (pzip_set_password(ctx, password.c_str()) != PZIP_OK) {
            std::fprintf(stderr, "failed to configure password for decryption\n");
            pzip_destroy(ctx);
            return 1;
        }
    }

    pzip_status_t opStatus = PZIP_OK;
    if (!singleEntry.empty()) {
        opStatus = pzip_extract_entry(ctx, archivePath.c_str(), singleEntry.c_str(), outputTarget.c_str());
        if (opStatus != PZIP_OK) {
            if (pzip_last_error(ctx, &errCode, &errMsg) == PZIP_OK) {
                std::fprintf(stderr, "failed to extract entry '%s': code=%" PRId32 " msg=%s\n",
                             singleEntry.c_str(), errCode, errMsg != NULL ? errMsg : "<null>");
            } else {
                std::fprintf(stderr, "failed to extract entry '%s'\n", singleEntry.c_str());
            }
            pzip_destroy(ctx);
            return 1;
        }
    } else {
        opStatus = pzip_extract_archive(ctx, archivePath.c_str(), outputTarget.c_str());
        if (opStatus != PZIP_OK) {
            if (pzip_last_error(ctx, &errCode, &errMsg) == PZIP_OK) {
                std::fprintf(stderr, "unzip failed: code=%" PRId32 " msg=%s\n",
                             errCode, errMsg != NULL ? errMsg : "<null>");
            } else {
                std::fprintf(stderr, "unzip failed\n");
            }
            pzip_destroy(ctx);
            return 1;
        }
    }

    pzip_destroy(ctx);
    return 0;
}
