#include "codec/zstd_codec.h"

#include <new>

#include "zstd.h"

namespace pzip {
namespace codec {
namespace {

struct ZstdCodecContext {
    int m_level;

    ZstdCodecContext() : m_level(3) {}
};

void* Create(void* user) {
    ZstdCodecContext* ctx = new (std::nothrow) ZstdCodecContext();
    if (ctx == NULL) {
        return NULL;
    }
    if (user != NULL) {
        ctx->m_level = *reinterpret_cast<const int*>(user);
    }
    return ctx;
}

void Destroy(void* codecCtx) {
    ZstdCodecContext* ctx = reinterpret_cast<ZstdCodecContext*>(codecCtx);
    delete ctx;
}

pzip_status_t Compress(void* codecCtx, const uint8_t* src, size_t srcSize, uint8_t* dst,
                       size_t dstCap, size_t* dstSize) {
    if (dstSize == NULL || dst == NULL || src == NULL) {
        return PZIP_E_INVALID_ARG;
    }

    const ZstdCodecContext* ctx = reinterpret_cast<const ZstdCodecContext*>(codecCtx);
    const int level = (ctx == NULL) ? 3 : ctx->m_level;
    const size_t ret = ZSTD_compress(dst, dstCap, src, srcSize, level);
    if (ZSTD_isError(ret)) {
        return PZIP_E_CODEC;
    }
    *dstSize = ret;
    return PZIP_OK;
}

pzip_status_t Decompress(void* codecCtx, const uint8_t* src, size_t srcSize, uint8_t* dst,
                         size_t dstCap, size_t* dstSize) {
    (void)codecCtx;
    if (dstSize == NULL || dst == NULL || src == NULL) {
        return PZIP_E_INVALID_ARG;
    }

    const size_t ret = ZSTD_decompress(dst, dstCap, src, srcSize);
    if (ZSTD_isError(ret)) {
        return PZIP_E_CODEC;
    }
    *dstSize = ret;
    return PZIP_OK;
}

size_t Bound(void* codecCtx, size_t srcSize) {
    (void)codecCtx;
    return ZSTD_compressBound(srcSize);
}

const char* Name(void* codecCtx) {
    (void)codecCtx;
    return "zstd";
}

}  // namespace

pzip_codec_vtable_t CreateZstdCodecVtable() {
    pzip_codec_vtable_t vtable;
    vtable.abi_version = PZIP_ABI_VERSION;
    vtable.zip_method = 93;
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
