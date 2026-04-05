#include "codec/lz4_codec.h"

#include <limits>
#include <new>

#include "lz4.h"

namespace pzip {
namespace codec {
namespace {

static const uint16_t kPzipLz4Method = 0x4C34u;

void* Create(void* user) {
    (void)user;
    return reinterpret_cast<void*>(1);
}

void Destroy(void* codecCtx) {
    (void)codecCtx;
}

pzip_status_t Compress(void* codecCtx, const uint8_t* src, size_t srcSize, uint8_t* dst,
                       size_t dstCap, size_t* dstSize) {
    (void)codecCtx;
    if (dstSize == NULL) {
        return PZIP_E_INVALID_ARG;
    }
    if (srcSize == 0) {
        *dstSize = 0;
        return PZIP_OK;
    }
    if (src == NULL || dst == NULL) {
        return PZIP_E_INVALID_ARG;
    }
    if (srcSize > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        dstCap > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return PZIP_E_NOT_SUPPORTED;
    }

    const int written =
        LZ4_compress_default(reinterpret_cast<const char*>(src), reinterpret_cast<char*>(dst),
                             static_cast<int>(srcSize), static_cast<int>(dstCap));
    if (written <= 0) {
        return PZIP_E_CODEC;
    }
    *dstSize = static_cast<size_t>(written);
    return PZIP_OK;
}

pzip_status_t Decompress(void* codecCtx, const uint8_t* src, size_t srcSize, uint8_t* dst,
                         size_t dstCap, size_t* dstSize) {
    (void)codecCtx;
    if (dstSize == NULL) {
        return PZIP_E_INVALID_ARG;
    }
    if (srcSize == 0) {
        *dstSize = 0;
        return PZIP_OK;
    }
    if (src == NULL || dst == NULL) {
        return PZIP_E_INVALID_ARG;
    }
    if (srcSize > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        dstCap > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return PZIP_E_NOT_SUPPORTED;
    }

    const int decoded =
        LZ4_decompress_safe(reinterpret_cast<const char*>(src), reinterpret_cast<char*>(dst),
                            static_cast<int>(srcSize), static_cast<int>(dstCap));
    if (decoded < 0) {
        return PZIP_E_CODEC;
    }
    *dstSize = static_cast<size_t>(decoded);
    return PZIP_OK;
}

size_t Bound(void* codecCtx, size_t srcSize) {
    (void)codecCtx;
    if (srcSize == 0) {
        return 1;
    }
    if (srcSize > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return 0;
    }
    return static_cast<size_t>(LZ4_compressBound(static_cast<int>(srcSize)));
}

const char* Name(void* codecCtx) {
    (void)codecCtx;
    return "lz4";
}

}  // namespace

pzip_codec_vtable_t CreateLz4CodecVtable() {
    pzip_codec_vtable_t vtable;
    vtable.abi_version = PZIP_ABI_VERSION;
    vtable.zip_method = kPzipLz4Method;
    vtable.reserved0 = 0;
    vtable.flags = PZIP_CODEC_FLAG_ZIP_COMPATIBLE;
    vtable.create = Create;
    vtable.destroy = Destroy;
    vtable.compress = Compress;
    vtable.decompress = Decompress;
    vtable.bound = Bound;
    vtable.name = Name;
    return vtable;
}

}  // namespace codec
}  // namespace pzip