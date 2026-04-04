#include "codec/zlib_codec.h"

#include <new>

#include "zlib.h"

namespace pzip {
namespace codec {
namespace {

struct ZlibCodecContext {
    int m_level;

    ZlibCodecContext() : m_level(Z_DEFAULT_COMPRESSION) {}
};

void* Create(void* user) {
    ZlibCodecContext* ctx = new (std::nothrow) ZlibCodecContext();
    if (ctx == NULL) {
        return NULL;
    }
    if (user != NULL) {
        ctx->m_level = *reinterpret_cast<const int*>(user);
    }
    return ctx;
}

void Destroy(void* codecCtx) {
    ZlibCodecContext* ctx = reinterpret_cast<ZlibCodecContext*>(codecCtx);
    delete ctx;
}

pzip_status_t Compress(void* codecCtx, const uint8_t* src, size_t srcSize, uint8_t* dst,
                       size_t dstCap, size_t* dstSize) {
    if (dstSize == NULL || dst == NULL || src == NULL) {
        return PZIP_E_INVALID_ARG;
    }

    const ZlibCodecContext* ctx = reinterpret_cast<const ZlibCodecContext*>(codecCtx);
    uLongf outLen = static_cast<uLongf>(dstCap);
    const int ret = compress2(reinterpret_cast<Bytef*>(dst), &outLen,
                              reinterpret_cast<const Bytef*>(src), static_cast<uLong>(srcSize),
                              (ctx == NULL) ? Z_DEFAULT_COMPRESSION : ctx->m_level);
    if (ret != Z_OK) {
        return PZIP_E_CODEC;
    }
    *dstSize = static_cast<size_t>(outLen);
    return PZIP_OK;
}

pzip_status_t Decompress(void* codecCtx, const uint8_t* src, size_t srcSize, uint8_t* dst,
                         size_t dstCap, size_t* dstSize) {
    (void)codecCtx;
    if (dstSize == NULL || dst == NULL || src == NULL) {
        return PZIP_E_INVALID_ARG;
    }

    uLongf outLen = static_cast<uLongf>(dstCap);
    const int ret = uncompress(reinterpret_cast<Bytef*>(dst), &outLen,
                               reinterpret_cast<const Bytef*>(src), static_cast<uLong>(srcSize));
    if (ret != Z_OK) {
        return PZIP_E_CODEC;
    }
    *dstSize = static_cast<size_t>(outLen);
    return PZIP_OK;
}

size_t Bound(void* codecCtx, size_t srcSize) {
    (void)codecCtx;
    return static_cast<size_t>(compressBound(static_cast<uLong>(srcSize)));
}

const char* Name(void* codecCtx) {
    (void)codecCtx;
    return "zlib-deflate";
}

}  // namespace

pzip_codec_vtable_t CreateZlibCodecVtable() {
    pzip_codec_vtable_t vtable;
    vtable.abi_version = PZIP_ABI_VERSION;
    vtable.zip_method = 8;
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
