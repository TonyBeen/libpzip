#include <errno.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
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

void* CreateSlowCodec(void*) {
    return reinterpret_cast<void*>(0x1);
}

void DestroySlowCodec(void*) {}

pzip_status_t CompressSlowCodec(void*, const uint8_t* src, size_t srcSize, uint8_t* dst,
                                size_t dstCap, size_t* dstSize) {
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    if (dstSize == NULL) {
        return PZIP_E_INVALID_ARG;
    }
    if (dstCap < srcSize) {
        return PZIP_E_NO_MEMORY;
    }
    if (srcSize > 0 && src != NULL && dst != NULL) {
        std::memcpy(dst, src, srcSize);
    }
    *dstSize = srcSize;
    return PZIP_OK;
}

size_t BoundSlowCodec(void*, size_t srcSize) {
    return srcSize + 16;
}

const char* NameSlowCodec(void*) {
    return "slow-copy";
}

bool MakeInputs(void) {
    static const char* kSeeds[] = {
        "cancel-alpha-0123456789\n",
        "cancel-beta-abcdefghij\n",
        "cancel-gamma-klmnopqrst\n",
        "cancel-delta-uvwxyz0123\n",
        "cancel-epsilon-456789abcd\n",
        "cancel-zeta-efghijklmn\n",
    };
    if (!EnsureDir("pzip_test_cancel") || !EnsureDir("pzip_test_cancel/input")) {
        return false;
    }
    for (size_t i = 0; i < sizeof(kSeeds) / sizeof(kSeeds[0]); ++i) {
        char path[128];
        std::snprintf(path, sizeof(path), "pzip_test_cancel/input/file_%02u.txt",
                      static_cast<unsigned>(i));
        if (!WritePatternFile(path, kSeeds[i], 8192)) {
            return false;
        }
    }
    return true;
}

}  // namespace

int main() {
    pzip_ctx_t* ctx = NULL;
    pzip_options_t opt;
    pzip_codec_vtable_t codec;
    pzip_status_t runStatus = PZIP_OK;
    int32_t lastCode = 0;
    const char* lastMessage = NULL;
    bool messageOk = false;

    if (!MakeInputs()) {
        return 20;
    }

    std::memset(&opt, 0, sizeof(opt));
    opt.abi_version = PZIP_ABI_VERSION;
    opt.thread_count = 2;
    opt.chunk_size_kb = 4;
    opt.enable_solid_mode = 0;
    opt.max_file_count = 64;
    opt.max_total_input_bytes = 1ULL << 27;

    if (pzip_create(&opt, &ctx) != PZIP_OK) {
        return 21;
    }

    std::memset(&codec, 0, sizeof(codec));
    codec.abi_version = PZIP_ABI_VERSION;
    codec.zip_method = 8;
    codec.flags = PZIP_CODEC_FLAG_ZIP_COMPATIBLE;
    codec.create = &CreateSlowCodec;
    codec.destroy = &DestroySlowCodec;
    codec.compress = &CompressSlowCodec;
    codec.bound = &BoundSlowCodec;
    codec.name = &NameSlowCodec;

    if (pzip_set_codec(ctx, &codec, NULL) != PZIP_OK) {
        pzip_destroy(ctx);
        return 22;
    }
    if (pzip_open_archive(ctx, "pzip_test_cancel/out.zip") != PZIP_OK) {
        pzip_destroy(ctx);
        return 23;
    }
    if (pzip_add_path(ctx, "pzip_test_cancel/input", "cancel") != PZIP_OK) {
        pzip_destroy(ctx);
        return 24;
    }

    std::thread runThread([&ctx, &runStatus]() { runStatus = pzip_run(ctx); });
    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    if (pzip_cancel(ctx) != PZIP_OK) {
        runThread.join();
        pzip_destroy(ctx);
        return 25;
    }

    runThread.join();
    pzip_last_error(ctx, &lastCode, &lastMessage);
    messageOk = lastMessage != NULL && std::strcmp(lastMessage, "compression canceled or failed") == 0;
    pzip_destroy(ctx);

    if (runStatus != PZIP_E_CANCELED) {
        return 26;
    }
    if (lastCode != PZIP_E_CANCELED) {
        return 27;
    }
    if (!messageOk) {
        return 28;
    }
    return 0;
}