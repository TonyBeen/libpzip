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
    if (fclose(out) != 0) {
        return 0;
    }
    return 1;
}

static uint16_t ReadLe16(const uint8_t* ptr) {
    return (uint16_t)(ptr[0] | ((uint16_t)ptr[1] << 8));
}

static uint32_t ReadLe32(const uint8_t* ptr) {
    return (uint32_t)(ptr[0] | ((uint32_t)ptr[1] << 8) | ((uint32_t)ptr[2] << 16) |
                      ((uint32_t)ptr[3] << 24));
}

static int ReadWholeFile(const char* path, uint8_t** outData, size_t* outSize) {
    FILE* in = fopen(path, "rb");
    long size;
    uint8_t* buffer;

    if (in == NULL || outData == NULL || outSize == NULL) {
        return 0;
    }
    if (fseek(in, 0, SEEK_END) != 0) {
        fclose(in);
        return 0;
    }
    size = ftell(in);
    if (size < 0 || fseek(in, 0, SEEK_SET) != 0) {
        fclose(in);
        return 0;
    }

    buffer = (uint8_t*)malloc((size_t)size);
    if (buffer == NULL && size > 0) {
        fclose(in);
        return 0;
    }
    if (size > 0 && fread(buffer, 1, (size_t)size, in) != (size_t)size) {
        free(buffer);
        fclose(in);
        return 0;
    }
    fclose(in);
    *outData = buffer;
    *outSize = (size_t)size;
    return 1;
}

static int ValidateArchive(const char* archivePath) {
    static const char* kExpectedNames[] = {
        "case/alpha.txt",
        "case/beta.txt",
        "case/gamma.txt",
    };
    uint8_t* data = NULL;
    size_t size = 0;
    size_t eocdOffset;
    uint32_t dirOffset;
    uint16_t entryCount;
    size_t i;

    if (!ReadWholeFile(archivePath, &data, &size) || size < 22) {
        return 0;
    }

    eocdOffset = size - 22;
    if (ReadLe32(data + eocdOffset) != 0x06054b50U) {
        free(data);
        return 0;
    }

    entryCount = ReadLe16(data + eocdOffset + 10);
    dirOffset = ReadLe32(data + eocdOffset + 16);
    if (entryCount != 3 || dirOffset >= size) {
        free(data);
        return 0;
    }

    for (i = 0; i < entryCount; ++i) {
        const uint8_t* header = data + dirOffset;
        uint16_t nameLen;
        uint16_t extraLen;
        uint16_t commentLen;
        char nameBuf[64];

        if (dirOffset + 46 > size || ReadLe32(header) != 0x02014b50U) {
            free(data);
            return 0;
        }

        nameLen = ReadLe16(header + 28);
        extraLen = ReadLe16(header + 30);
        commentLen = ReadLe16(header + 32);
        if (nameLen >= sizeof(nameBuf) || dirOffset + 46 + nameLen + extraLen + commentLen > size) {
            free(data);
            return 0;
        }

        memcpy(nameBuf, header + 46, nameLen);
        nameBuf[nameLen] = '\0';
        if (strcmp(nameBuf, kExpectedNames[i]) != 0) {
            free(data);
            return 0;
        }

        dirOffset += 46U + (uint32_t)nameLen + (uint32_t)extraLen + (uint32_t)commentLen;
    }

    free(data);
    return 1;
}

int main(void) {
    const char* root = "pzip_test_pipeline";
    const char* inputDir = "pzip_test_pipeline/input";
    const char* archivePath = "pzip_test_pipeline/out.zip";
    pzip_ctx_t* ctx = NULL;
    pzip_options_t opt;
    int i;

    if (!EnsureDir(root) || !EnsureDir(inputDir)) {
        return 10;
    }
    if (!WritePatternFile("pzip_test_pipeline/input/alpha.txt", "alpha-0123456789\n", 4096) ||
        !WritePatternFile("pzip_test_pipeline/input/beta.txt", "beta-abcdefghij\n", 3072) ||
        !WritePatternFile("pzip_test_pipeline/input/gamma.txt", "gamma-uvwxyz012\n", 2048)) {
        return 11;
    }

    for (i = 0; i < 10; ++i) {
        opt.reserve[i] = 0;
    }
    opt.abi_version = PZIP_ABI_VERSION;
    opt.thread_count = 4;
    opt.chunk_size_kb = 4;
    opt.enable_solid_mode = 0;
    opt.max_file_count = 16;
    opt.max_total_input_bytes = 1ULL << 26;

    if (pzip_create(&opt, &ctx) != PZIP_OK) {
        return 12;
    }
    if (pzip_open_archive(ctx, archivePath) != PZIP_OK) {
        pzip_destroy(ctx);
        return 13;
    }
    if (pzip_add_path(ctx, "pzip_test_pipeline/input/alpha.txt", "case") != PZIP_OK ||
        pzip_add_path(ctx, "pzip_test_pipeline/input/beta.txt", "case") != PZIP_OK ||
        pzip_add_path(ctx, "pzip_test_pipeline/input/gamma.txt", "case") != PZIP_OK) {
        pzip_destroy(ctx);
        return 14;
    }
    if (pzip_run(ctx) != PZIP_OK) {
        pzip_destroy(ctx);
        return 15;
    }
    pzip_destroy(ctx);

    if (!ValidateArchive(archivePath)) {
        return 16;
    }
    return 0;
}