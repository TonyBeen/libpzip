#include "crypto/sha1.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace pzip {
namespace crypto {

namespace {

static inline uint32_t Rol32(uint32_t value, int bits) {
    return (value << bits) | (value >> (32 - bits));
}

static inline void Encode32BE(uint8_t* dst, uint32_t val) {
    dst[0] = static_cast<uint8_t>((val >> 24) & 0xff);
    dst[1] = static_cast<uint8_t>((val >> 16) & 0xff);
    dst[2] = static_cast<uint8_t>((val >> 8) & 0xff);
    dst[3] = static_cast<uint8_t>(val & 0xff);
}

static inline uint32_t Decode32BE(const uint8_t* src) {
    return (static_cast<uint32_t>(src[0]) << 24) |
           (static_cast<uint32_t>(src[1]) << 16) |
           (static_cast<uint32_t>(src[2]) << 8) |
           static_cast<uint32_t>(src[3]);
}

void Sha1Transform(uint32_t state[5], const uint8_t buffer[64]) {
    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    uint32_t e = state[4];
    uint32_t w[80];

    for (int i = 0; i < 16; ++i) {
        w[i] = Decode32BE(&buffer[i * 4]);
    }
    for (int i = 16; i < 80; ++i) {
        w[i] = Rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    for (int i = 0; i < 20; ++i) {
        const uint32_t temp = Rol32(a, 5) + ((b & c) | (~b & d)) + e + 0x5A827999U + w[i];
        e = d;
        d = c;
        c = Rol32(b, 30);
        b = a;
        a = temp;
    }
    for (int i = 20; i < 40; ++i) {
        const uint32_t temp = Rol32(a, 5) + (b ^ c ^ d) + e + 0x6ED9EBA1U + w[i];
        e = d;
        d = c;
        c = Rol32(b, 30);
        b = a;
        a = temp;
    }
    for (int i = 40; i < 60; ++i) {
        const uint32_t temp = Rol32(a, 5) + ((b & c) | (b & d) | (c & d)) + e + 0x8F1BBCDCU + w[i];
        e = d;
        d = c;
        c = Rol32(b, 30);
        b = a;
        a = temp;
    }
    for (int i = 60; i < 80; ++i) {
        const uint32_t temp = Rol32(a, 5) + (b ^ c ^ d) + e + 0xCA62C1D6U + w[i];
        e = d;
        d = c;
        c = Rol32(b, 30);
        b = a;
        a = temp;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

}  // namespace

void Sha1Init(Sha1Context* ctx) {
    if (ctx == NULL) return;
    ctx->state[0] = 0x67452301U;
    ctx->state[1] = 0xEFCDAB89U;
    ctx->state[2] = 0x98BADCFEU;
    ctx->state[3] = 0x10325476U;
    ctx->state[4] = 0xC3D2E1F0U;
    ctx->count = 0;
}

void Sha1Update(Sha1Context* ctx, const uint8_t* data, size_t len) {
    if (ctx == NULL || data == NULL || len == 0) return;

    size_t bufferIndex = static_cast<size_t>(ctx->count % 64);
    ctx->count += len;

    size_t dataPos = 0;
    if (bufferIndex > 0) {
        const size_t needed = 64 - bufferIndex;
        if (len >= needed) {
            std::memcpy(&ctx->buffer[bufferIndex], data, needed);
            Sha1Transform(ctx->state, ctx->buffer);
            dataPos += needed;
            bufferIndex = 0;
        } else {
            std::memcpy(&ctx->buffer[bufferIndex], data, len);
            return;
        }
    }

    while (dataPos + 64 <= len) {
        Sha1Transform(ctx->state, &data[dataPos]);
        dataPos += 64;
    }

    if (dataPos < len) {
        std::memcpy(ctx->buffer, &data[dataPos], len - dataPos);
    }
}

void Sha1Final(Sha1Context* ctx, uint8_t digest[20]) {
    if (ctx == NULL || digest == NULL) return;

    const uint64_t totalBits = ctx->count * 8;
    const size_t bufferIndex = static_cast<size_t>(ctx->count % 64);
    ctx->buffer[bufferIndex] = 0x80;

    if (bufferIndex >= 56) {
        std::memset(&ctx->buffer[bufferIndex + 1], 0, 63 - bufferIndex);
        Sha1Transform(ctx->state, ctx->buffer);
        std::memset(ctx->buffer, 0, 56);
    } else {
        std::memset(&ctx->buffer[bufferIndex + 1], 0, 55 - bufferIndex);
    }

    for (int i = 7; i >= 0; --i) {
        ctx->buffer[56 + (7 - i)] = static_cast<uint8_t>((totalBits >> (i * 8)) & 0xff);
    }
    Sha1Transform(ctx->state, ctx->buffer);

    for (int i = 0; i < 5; ++i) {
        Encode32BE(&digest[i * 4], ctx->state[i]);
    }
}

void Sha1(const uint8_t* data, size_t len, uint8_t digest[20]) {
    Sha1Context ctx;
    Sha1Init(&ctx);
    Sha1Update(&ctx, data, len);
    Sha1Final(&ctx, digest);
}

void HmacSha1(const uint8_t* key, size_t keyLen,
              const uint8_t* data, size_t dataLen,
              uint8_t out[20]) {
    uint8_t keyPad[64];
    std::memset(keyPad, 0, sizeof(keyPad));

    if (keyLen > 64) {
        Sha1(key, keyLen, keyPad);
    } else if (key != NULL && keyLen > 0) {
        std::memcpy(keyPad, key, keyLen);
    }

    uint8_t iPad[64];
    uint8_t oPad[64];
    for (int i = 0; i < 64; ++i) {
        iPad[i] = keyPad[i] ^ 0x36;
        oPad[i] = keyPad[i] ^ 0x5C;
    }

    uint8_t innerHash[20];
    Sha1Context innerCtx;
    Sha1Init(&innerCtx);
    Sha1Update(&innerCtx, iPad, 64);
    Sha1Update(&innerCtx, data, dataLen);
    Sha1Final(&innerCtx, innerHash);

    Sha1Context outerCtx;
    Sha1Init(&outerCtx);
    Sha1Update(&outerCtx, oPad, 64);
    Sha1Update(&outerCtx, innerHash, 20);
    Sha1Final(&outerCtx, out);
}

void Pbkdf2HmacSha1(const uint8_t* password, size_t passLen,
                    const uint8_t* salt, size_t saltLen,
                    uint32_t iterations,
                    uint8_t* outKey, size_t keyLen) {
    if (outKey == NULL || keyLen == 0 || iterations == 0) return;

    size_t generated = 0;
    uint32_t blockIndex = 1;

    std::vector<uint8_t> saltBlock(saltLen + 4);
    if (salt != NULL && saltLen > 0) {
        std::memcpy(&saltBlock[0], salt, saltLen);
    }

    while (generated < keyLen) {
        saltBlock[saltLen + 0] = static_cast<uint8_t>((blockIndex >> 24) & 0xff);
        saltBlock[saltLen + 1] = static_cast<uint8_t>((blockIndex >> 16) & 0xff);
        saltBlock[saltLen + 2] = static_cast<uint8_t>((blockIndex >> 8) & 0xff);
        saltBlock[saltLen + 3] = static_cast<uint8_t>(blockIndex & 0xff);

        uint8_t u[20];
        uint8_t f[20];
        HmacSha1(password, passLen, &saltBlock[0], saltBlock.size(), u);
        std::memcpy(f, u, 20);

        for (uint32_t iter = 1; iter < iterations; ++iter) {
            HmacSha1(password, passLen, u, 20, u);
            for (int k = 0; k < 20; ++k) {
                f[k] ^= u[k];
            }
        }

        const size_t toCopy = std::min<size_t>(20, keyLen - generated);
        std::memcpy(outKey + generated, f, toCopy);
        generated += toCopy;
        ++blockIndex;
    }
}

}  // namespace crypto
}  // namespace pzip
