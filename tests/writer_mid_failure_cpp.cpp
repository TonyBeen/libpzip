#include <errno.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#include <direct.h>
#define PZIP_TEST_MKDIR(path) _mkdir(path)
#define PZIP_SET_ENV(name, value) _putenv_s(name, value)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define PZIP_TEST_MKDIR(path) mkdir(path, 0755)
#define PZIP_SET_ENV(name, value) setenv(name, value, 1)
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

void ClearFailWriteEnv() {
#if defined(_WIN32)
    PZIP_SET_ENV("PZIP_TEST_FAIL_WRITE_AFTER_BYTES", "");
#else
    unsetenv("PZIP_TEST_FAIL_WRITE_AFTER_BYTES");
#endif
}

}  // namespace

int main() {
    const char* rootDir = "pzip_writer_failure_case";
    const char* inputDir = "pzip_writer_failure_case/input";
    const char* archivePath = "pzip_writer_failure_case/out.zip";
    pzip_ctx_t* ctx = NULL;
    pzip_options_t opt;
    pzip_status_t status;
    int32_t lastCode = 0;
    const char* lastMessage = NULL;

    if (!EnsureDir(rootDir) || !EnsureDir(inputDir)) {
        return 60;
    }
    if (!WritePatternFile("pzip_writer_failure_case/input/payload.txt", "writer-failure-0123456789\n", 4096)) {
        return 61;
    }

    std::memset(&opt, 0, sizeof(opt));
    opt.abi_version = PZIP_ABI_VERSION;
    opt.thread_count = 2;
    opt.chunk_size_kb = 4;
    opt.enable_solid_mode = 0;
    opt.max_file_count = 8;
    opt.max_total_input_bytes = 1ULL << 25;

    if (pzip_create(&opt, &ctx) != PZIP_OK) {
        return 62;
    }
    if (pzip_open_archive(ctx, archivePath) != PZIP_OK) {
        pzip_destroy(ctx);
        return 63;
    }
    if (pzip_add_path(ctx, inputDir, "writer") != PZIP_OK) {
        pzip_destroy(ctx);
        return 64;
    }

    if (PZIP_SET_ENV("PZIP_TEST_FAIL_WRITE_AFTER_BYTES", "128") != 0) {
        pzip_destroy(ctx);
        return 65;
    }

    status = pzip_run(ctx);
    pzip_last_error(ctx, &lastCode, &lastMessage);
    ClearFailWriteEnv();
    pzip_destroy(ctx);

    if (status != PZIP_E_IO) {
        return 66;
    }
    if (lastCode != PZIP_E_IO) {
        return 67;
    }
    if (lastMessage == NULL || lastMessage[0] == '\0') {
        return 68;
    }
    return 0;
}