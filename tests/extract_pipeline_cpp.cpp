#include <errno.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

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

bool ReadWholeFile(const char* path, std::vector<uint8_t>* out) {
    FILE* in = std::fopen(path, "rb");
    long sz = 0;
    if (in == NULL || out == NULL) {
        return false;
    }
    if (std::fseek(in, 0, SEEK_END) != 0) {
        std::fclose(in);
        return false;
    }
    sz = std::ftell(in);
    if (sz < 0 || std::fseek(in, 0, SEEK_SET) != 0) {
        std::fclose(in);
        return false;
    }
    out->resize(static_cast<size_t>(sz));
    if (sz > 0 && std::fread(&(*out)[0], 1, static_cast<size_t>(sz), in) != static_cast<size_t>(sz)) {
        std::fclose(in);
        return false;
    }
    std::fclose(in);
    return true;
}

}  // namespace

int main() {
    pzip_ctx_t* ctx = NULL;
    pzip_options_t opt;
    std::vector<uint8_t> src;
    std::vector<uint8_t> dst;

    if (!EnsureDir("pzip_extract_case") || !EnsureDir("pzip_extract_case/input") ||
        !EnsureDir("pzip_extract_case/output")) {
        return 80;
    }

    if (!WritePatternFile("pzip_extract_case/input/file.txt", "extract-verify-abcdefghijklmnopqrstuvwxyz\n", 4096)) {
        return 81;
    }

    std::memset(&opt, 0, sizeof(opt));
    opt.abi_version = PZIP_ABI_VERSION;
    opt.thread_count = 2;
    opt.chunk_size_kb = 4;
    opt.enable_solid_mode = 0;
    opt.max_file_count = 16;
    opt.max_total_input_bytes = 1ULL << 26;

    if (pzip_create(&opt, &ctx) != PZIP_OK) {
        return 82;
    }
    if (pzip_open_archive(ctx, "pzip_extract_case/out.zip") != PZIP_OK) {
        pzip_destroy(ctx);
        return 83;
    }
    if (pzip_add_path(ctx, "pzip_extract_case/input", "in") != PZIP_OK) {
        pzip_destroy(ctx);
        return 84;
    }
    if (pzip_run(ctx) != PZIP_OK) {
        pzip_destroy(ctx);
        return 85;
    }

    if (pzip_extract_archive(ctx, "pzip_extract_case/out.zip", "pzip_extract_case/output") != PZIP_OK) {
        pzip_destroy(ctx);
        return 86;
    }

    if (!ReadWholeFile("pzip_extract_case/input/file.txt", &src) ||
        !ReadWholeFile("pzip_extract_case/output/in/file.txt", &dst)) {
        pzip_destroy(ctx);
        return 87;
    }

    pzip_destroy(ctx);

    if (src.size() != dst.size()) {
        return 88;
    }
    if (!src.empty() && std::memcmp(&src[0], &dst[0], src.size()) != 0) {
        return 89;
    }
    return 0;
}
