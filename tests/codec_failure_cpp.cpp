#include <errno.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

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

void* CreateFailCodec(void*) {
    return reinterpret_cast<void*>(0x1);
}

void DestroyFailCodec(void*) {}

pzip_status_t CompressFailCodec(void*, const uint8_t*, size_t, uint8_t*, size_t, size_t*) {
    return PZIP_E_CODEC;
}

size_t BoundFailCodec(void*, size_t srcSize) {
    return srcSize + 16;
}

const char* NameFailCodec(void*) {
    return "always-fail";
}

}  // namespace

int main() {
    const char* rootDir = "pzip_codec_failure_case";
    const char* inputDir = "pzip_codec_failure_case/input";
    const char* archivePath = "pzip_codec_failure_case/out.zip";
    pzip_ctx_t* ctx = NULL;
    pzip_options_t opt;
    pzip_codec_vtable_t codec;
    pzip_status_t status;
    int32_t lastCode = 0;
    const char* lastMessage = NULL;

    if (!EnsureDir(rootDir) || !EnsureDir(inputDir)) {
        return 30;
    }
    if (!WritePatternFile("pzip_codec_failure_case/input/payload.txt", "codec-failure-0123456789\n", 1024)) {
        return 31;
    }

    std::memset(&opt, 0, sizeof(opt));
    opt.abi_version = PZIP_ABI_VERSION;
    opt.thread_count = 2;
    opt.chunk_size_kb = 4;
    opt.enable_solid_mode = 0;
    opt.max_file_count = 8;
    opt.max_total_input_bytes = 1ULL << 24;

    if (pzip_create(&opt, &ctx) != PZIP_OK) {
        return 32;
    }

    std::memset(&codec, 0, sizeof(codec));
    codec.abi_version = PZIP_ABI_VERSION;
    codec.zip_method = 8;
    codec.flags = PZIP_CODEC_FLAG_ZIP_COMPATIBLE;
    codec.create = &CreateFailCodec;
    codec.destroy = &DestroyFailCodec;
    codec.compress = &CompressFailCodec;
    codec.bound = &BoundFailCodec;
    codec.name = &NameFailCodec;

    if (pzip_set_codec(ctx, &codec, NULL) != PZIP_OK) {
        pzip_destroy(ctx);
        return 33;
    }
    if (pzip_open_archive(ctx, archivePath) != PZIP_OK) {
        pzip_destroy(ctx);
        return 34;
    }
    if (pzip_add_path(ctx, inputDir, "codec") != PZIP_OK) {
        pzip_destroy(ctx);
        return 35;
    }

    status = pzip_run(ctx);
    pzip_last_error(ctx, &lastCode, &lastMessage);
    pzip_destroy(ctx);

    if (status != PZIP_E_CODEC) {
        return 36;
    }
    if (lastCode != PZIP_E_CODEC) {
        return 37;
    }
    if (lastMessage == NULL || lastMessage[0] == '\0') {
        return 38;
    }
    return 0;
}