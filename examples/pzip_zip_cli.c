#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pzip.h"

static void PrintUsage(const char* argv0) {
    fprintf(stderr, "Usage: %s <archive.zip> <input_path> [entry_prefix] [thread_count]\n", argv0);
}

int main(int argc, char** argv) {
    pzip_ctx_t* ctx = NULL;
    pzip_options_t opt;
    const char* archivePath;
    const char* inputPath;
    const char* entryPrefix;
    int32_t errCode = 0;
    const char* errMsg = NULL;

    if (argc < 3 || argc > 5) {
        PrintUsage(argc > 0 ? argv[0] : "pzip-zip");
        return 2;
    }

    archivePath = argv[1];
    inputPath = argv[2];
    entryPrefix = argc >= 4 ? argv[3] : "";

    memset(&opt, 0, sizeof(opt));
    opt.abi_version = PZIP_ABI_VERSION;
    opt.thread_count = 0;
    opt.chunk_size_kb = 256;
    opt.enable_solid_mode = 0;
    opt.max_file_count = 1000000U;
    opt.max_total_input_bytes = 1ULL << 40;

    if (argc >= 5) {
        long threads = strtol(argv[4], NULL, 10);
        if (threads < 0 || threads > 1024) {
            fprintf(stderr, "Invalid thread_count: %s\n", argv[4]);
            return 2;
        }
        opt.thread_count = (uint32_t)threads;
    }

    if (pzip_create(&opt, &ctx) != PZIP_OK) {
        fprintf(stderr, "pzip_create failed\n");
        return 1;
    }

    if (pzip_open_archive(ctx, archivePath) != PZIP_OK ||
        pzip_add_path(ctx, inputPath, entryPrefix) != PZIP_OK ||
        pzip_run(ctx) != PZIP_OK) {
        if (pzip_last_error(ctx, &errCode, &errMsg) == PZIP_OK) {
            fprintf(stderr, "zip failed: code=%d msg=%s\n", errCode, errMsg != NULL ? errMsg : "<null>");
        } else {
            fprintf(stderr, "zip failed\n");
        }
        pzip_destroy(ctx);
        return 1;
    }

    pzip_destroy(ctx);
    return 0;
}
