#include "codec/zlib_codec.h"

#include <cstring>
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
    if (dstSize == NULL) {
        return PZIP_E_INVALID_ARG;
    }
    if (srcSize == 0) {
        *dstSize = 0;
        return PZIP_OK;
    }
    if (dst == NULL || src == NULL) {
        return PZIP_E_INVALID_ARG;
    }

    const ZlibCodecContext* ctx = reinterpret_cast<const ZlibCodecContext*>(codecCtx);
    const int level = (ctx == NULL) ? Z_DEFAULT_COMPRESSION : ctx->m_level;

    z_stream strm;
    std::memset(&strm, 0, sizeof(strm));
    strm.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(src));
    strm.avail_in = static_cast<uInt>(srcSize);
    strm.next_out = reinterpret_cast<Bytef*>(dst);
    strm.avail_out = static_cast<uInt>(dstCap);

    // ZIP Method 8 requires raw Deflate (-MAX_WBITS)
    int ret = deflateInit2(&strm, level, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    if (ret != Z_OK) {
        return PZIP_E_CODEC;
    }

    ret = deflate(&strm, Z_FINISH);
    deflateEnd(&strm);

    if (ret != Z_STREAM_END) {
        return PZIP_E_CODEC;
    }
    *dstSize = static_cast<size_t>(strm.total_out);
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
    if (dst == NULL || src == NULL) {
        return PZIP_E_INVALID_ARG;
    }

    z_stream strm;
    std::memset(&strm, 0, sizeof(strm));
    strm.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(src));
    strm.avail_in = static_cast<uInt>(srcSize);
    strm.next_out = reinterpret_cast<Bytef*>(dst);
    strm.avail_out = static_cast<uInt>(dstCap);

    // Try raw Deflate (-MAX_WBITS) first for standard ZIP archives
    int ret = inflateInit2(&strm, -MAX_WBITS);
    if (ret != Z_OK) {
        return PZIP_E_CODEC;
    }
    ret = inflate(&strm, Z_FINISH);
    inflateEnd(&strm);

    if (ret != Z_STREAM_END) {
        // Fallback: try standard zlib wrapper (MAX_WBITS) in case stream had zlib headers
        std::memset(&strm, 0, sizeof(strm));
        strm.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(src));
        strm.avail_in = static_cast<uInt>(srcSize);
        strm.next_out = reinterpret_cast<Bytef*>(dst);
        strm.avail_out = static_cast<uInt>(dstCap);

        ret = inflateInit2(&strm, MAX_WBITS);
        if (ret != Z_OK) {
            return PZIP_E_CODEC;
        }
        ret = inflate(&strm, Z_FINISH);
        inflateEnd(&strm);
        if (ret != Z_STREAM_END) {
            return PZIP_E_CODEC;
        }
    }

    *dstSize = static_cast<size_t>(strm.total_out);
    return PZIP_OK;
}

size_t Bound(void* codecCtx, size_t srcSize) {
    (void)codecCtx;
    return static_cast<size_t>(compressBound(static_cast<uLong>(srcSize))) + 16U;
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
