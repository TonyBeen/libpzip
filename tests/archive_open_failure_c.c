#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#define PZIP_TEST_MKDIR(path) _mkdir(path)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define PZIP_TEST_MKDIR(path) mkdir(path, 0755)
#endif

#include "pzip.h"

static int EnsureDir(const char* path) {
    if (PZIP_TEST_MKDIR(path) == 0) {
        return 1;
    }
    return errno == EEXIST;
}

static int WritePatternFile(const char* path, const char* seed, size_t repeatCount) {
    FILE* out = fopen(path, "wb");
    size_t i;
    const size_t seedLen = strlen(seed);

    if (out == NULL) {
        return 0;
    }
    for (i = 0; i < repeatCount; ++i) {
        if (fwrite(seed, 1, seedLen, out) != seedLen) {
            fclose(out);
            return 0;
        }
    }
    return fclose(out) == 0;
}

int main(void) {
    pzip_ctx_t* ctx = NULL;
    pzip_options_t opt;
    pzip_status_t status;
    int32_t lastCode = 0;
    const char* lastMessage = NULL;

    if (!EnsureDir("pzip_test_archive_failure") || !EnsureDir("pzip_test_archive_failure/input")) {
        return 40;
    }
    if (!WritePatternFile("pzip_test_archive_failure/input/payload.txt", "archive-open-failure\n", 512)) {
        return 41;
    }

    memset(&opt, 0, sizeof(opt));
    opt.abi_version = PZIP_ABI_VERSION;
    opt.thread_count = 2;
    opt.chunk_size_kb = 4;
    opt.enable_solid_mode = 0;
    opt.max_file_count = 8;
    opt.max_total_input_bytes = 1ULL << 24;

    if (pzip_create(&opt, &ctx) != PZIP_OK) {
        return 42;
    }
    if (pzip_open_archive(ctx, "pzip_test_archive_failure/missing/out.zip") != PZIP_OK) {
        pzip_destroy(ctx);
        return 43;
    }
    if (pzip_add_path(ctx, "pzip_test_archive_failure/input", "archive") != PZIP_OK) {
        pzip_destroy(ctx);
        return 44;
    }

    status = pzip_run(ctx);
    pzip_last_error(ctx, &lastCode, &lastMessage);
    pzip_destroy(ctx);

    if (status != PZIP_E_IO) {
        return 45;
    }
    if (lastCode != PZIP_E_IO) {
        return 46;
    }
    if (lastMessage == NULL || lastMessage[0] == '\0') {
        return 47;
    }
    return 0;
}