#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pzip.h"

static void PrintUsage(const char* argv0) {
    fprintf(stderr, "Usage: %s <archive.zip> <output_dir>\n", argv0);
}

int main(int argc, char** argv) {
    pzip_ctx_t* ctx = NULL;
    pzip_options_t opt;
    int32_t errCode = 0;
    const char* errMsg = NULL;

    if (argc != 3) {
        PrintUsage(argc > 0 ? argv[0] : "pzip-unzip");
        return 2;
    }

    memset(&opt, 0, sizeof(opt));
    opt.abi_version = PZIP_ABI_VERSION;
    opt.thread_count = 0;
    opt.chunk_size_kb = 256;
    opt.enable_solid_mode = 0;
    opt.max_file_count = 1000000U;
    opt.max_total_input_bytes = 1ULL << 40;

    if (pzip_create(&opt, &ctx) != PZIP_OK) {
        fprintf(stderr, "pzip_create failed\n");
        return 1;
    }

    if (pzip_extract_archive(ctx, argv[1], argv[2]) != PZIP_OK) {
        if (pzip_last_error(ctx, &errCode, &errMsg) == PZIP_OK) {
            fprintf(stderr, "unzip failed: code=%d msg=%s\n", errCode, errMsg != NULL ? errMsg : "<null>");
        } else {
            fprintf(stderr, "unzip failed\n");
        }
        pzip_destroy(ctx);
        return 1;
    }

    pzip_destroy(ctx);
    return 0;
}
