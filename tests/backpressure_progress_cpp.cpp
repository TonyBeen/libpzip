#include <errno.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#include <direct.h>
#include <sys/stat.h>
#define PZIP_TEST_MKDIR(path) _mkdir(path)
#define PZIP_TEST_STAT _stat
#else
#include <sys/stat.h>
#include <sys/types.h>
#define PZIP_TEST_MKDIR(path) mkdir(path, 0755)
#define PZIP_TEST_STAT stat
#endif

#include "pzip.h"

namespace {

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

bool FileExistsAndNonEmpty(const char* path) {
    struct PZIP_TEST_STAT st;
    if (PZIP_TEST_STAT(path, &st) != 0) {
        return false;
    }
    return st.st_size > 0;
}

}  // namespace

int main() {
    const char* rootDir = "pzip_backpressure_case";
    const char* inputFile = "pzip_backpressure_case/input_large.txt";
    const char* archivePath = "pzip_backpressure_case/out.zip";
    pzip_ctx_t* ctx = NULL;
    pzip_options_t opt;
    pzip_status_t status;

    if (!EnsureDir(rootDir)) {
        return 50;
    }

    if (!WritePatternFile(inputFile, "backpressure-progress-0123456789abcdef\n", 12000)) {
        return 51;
    }

    std::memset(&opt, 0, sizeof(opt));
    opt.abi_version = PZIP_ABI_VERSION;
    opt.thread_count = 2;
    opt.chunk_size_kb = 4;
    opt.enable_solid_mode = 0;
    opt.max_file_count = 8;
    opt.max_total_input_bytes = 4ULL * 1024ULL;

    if (pzip_create(&opt, &ctx) != PZIP_OK) {
        return 52;
    }
    if (pzip_open_archive(ctx, archivePath) != PZIP_OK) {
        pzip_destroy(ctx);
        return 53;
    }

    // Add as a single file (not directory) so this test can keep max_total_input_bytes
    // intentionally tiny and still stress backpressure during run().
    if (pzip_add_path(ctx, inputFile, "bp") != PZIP_OK) {
        pzip_destroy(ctx);
        return 54;
    }

    status = pzip_run(ctx);
    pzip_destroy(ctx);

    if (status != PZIP_OK) {
        return 55;
    }
    if (!FileExistsAndNonEmpty(archivePath)) {
        return 56;
    }
    return 0;
}