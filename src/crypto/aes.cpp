#include "crypto/aes.h"

#include <algorithm>
#include <cstring>

namespace pzip {
namespace crypto {

namespace {

static const uint8_t kSBox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16
};

static const uint32_t kRcon[11] = {
    0x00000000U, 0x01000000U, 0x02000000U, 0x04000000U, 0x08000000U,
    0x10000000U, 0x20000000U, 0x40000000U, 0x80000000U, 0x1B000000U, 0x36000000U
};

static inline uint8_t Xtime(uint8_t x) {
    return static_cast<uint8_t>(((x << 1) ^ ((x & 0x80) ? 0x1BU : 0x00U)) & 0xFFU);
}

static inline uint32_t SubWord(uint32_t word) {
    return (static_cast<uint32_t>(kSBox[(word >> 24) & 0xFF]) << 24) |
           (static_cast<uint32_t>(kSBox[(word >> 16) & 0xFF]) << 16) |
           (static_cast<uint32_t>(kSBox[(word >> 8) & 0xFF]) << 8) |
           static_cast<uint32_t>(kSBox[word & 0xFF]);
}

static inline uint32_t RotWord(uint32_t word) {
    return (word << 8) | (word >> 24);
}

}  // namespace

bool AesSetEncryptKey(AesContext* ctx, const uint8_t* key, size_t keyLen) {
    if (ctx == NULL || key == NULL) return false;

    int nk = 0;
    if (keyLen == 16) {
        nk = 4;
        ctx->rounds = 10;
    } else if (keyLen == 32) {
        nk = 8;
        ctx->rounds = 14;
    } else {
        return false;
    }

    const int totalWords = 4 * (ctx->rounds + 1);
    for (int i = 0; i < nk; ++i) {
        ctx->roundKeys[i] = (static_cast<uint32_t>(key[4 * i]) << 24) |
                            (static_cast<uint32_t>(key[4 * i + 1]) << 16) |
                            (static_cast<uint32_t>(key[4 * i + 2]) << 8) |
                            static_cast<uint32_t>(key[4 * i + 3]);
    }

    for (int i = nk; i < totalWords; ++i) {
        uint32_t temp = ctx->roundKeys[i - 1];
        if (i % nk == 0) {
            temp = SubWord(RotWord(temp)) ^ kRcon[i / nk];
        } else if (nk > 6 && (i % nk == 4)) {
            temp = SubWord(temp);
        }
        ctx->roundKeys[i] = ctx->roundKeys[i - nk] ^ temp;
    }
    return true;
}

void AesEncryptBlock(const AesContext* ctx, const uint8_t in[16], uint8_t out[16]) {
    uint8_t state[4][4];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            state[r][c] = in[r + 4 * c];
        }
    }

    // Initial round key addition
    for (int c = 0; c < 4; ++c) {
        const uint32_t rk = ctx->roundKeys[c];
        state[0][c] ^= static_cast<uint8_t>((rk >> 24) & 0xFF);
        state[1][c] ^= static_cast<uint8_t>((rk >> 16) & 0xFF);
        state[2][c] ^= static_cast<uint8_t>((rk >> 8) & 0xFF);
        state[3][c] ^= static_cast<uint8_t>(rk & 0xFF);
    }

    // Rounds 1 to rounds - 1
    for (int round = 1; round < ctx->rounds; ++round) {
        // SubBytes
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                state[r][c] = kSBox[state[r][c]];
            }
        }

        // ShiftRows
        uint8_t temp;
        // row 1: shift 1
        temp = state[1][0];
        state[1][0] = state[1][1];
        state[1][1] = state[1][2];
        state[1][2] = state[1][3];
        state[1][3] = temp;
        // row 2: shift 2
        temp = state[2][0];
        state[2][0] = state[2][2];
        state[2][2] = temp;
        temp = state[2][1];
        state[2][1] = state[2][3];
        state[2][3] = temp;
        // row 3: shift 3
        temp = state[3][3];
        state[3][3] = state[3][2];
        state[3][2] = state[3][1];
        state[3][1] = state[3][0];
        state[3][0] = temp;

        // MixColumns
        for (int c = 0; c < 4; ++c) {
            const uint8_t a = state[0][c];
            const uint8_t b = state[1][c];
            const uint8_t c_val = state[2][c];
            const uint8_t d = state[3][c];
            state[0][c] = Xtime(a ^ b) ^ b ^ c_val ^ d;
            state[1][c] = Xtime(b ^ c_val) ^ c_val ^ d ^ a;
            state[2][c] = Xtime(c_val ^ d) ^ d ^ a ^ b;
            state[3][c] = Xtime(d ^ a) ^ a ^ b ^ c_val;
        }

        // AddRoundKey
        for (int c = 0; c < 4; ++c) {
            const uint32_t rk = ctx->roundKeys[round * 4 + c];
            state[0][c] ^= static_cast<uint8_t>((rk >> 24) & 0xFF);
            state[1][c] ^= static_cast<uint8_t>((rk >> 16) & 0xFF);
            state[2][c] ^= static_cast<uint8_t>((rk >> 8) & 0xFF);
            state[3][c] ^= static_cast<uint8_t>(rk & 0xFF);
        }
    }

    // Final round (no MixColumns)
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            state[r][c] = kSBox[state[r][c]];
        }
    }

    // ShiftRows
    uint8_t temp;
    temp = state[1][0];
    state[1][0] = state[1][1];
    state[1][1] = state[1][2];
    state[1][2] = state[1][3];
    state[1][3] = temp;

    temp = state[2][0];
    state[2][0] = state[2][2];
    state[2][2] = temp;
    temp = state[2][1];
    state[2][1] = state[2][3];
    state[2][3] = temp;

    temp = state[3][3];
    state[3][3] = state[3][2];
    state[3][2] = state[3][1];
    state[3][1] = state[3][0];
    state[3][0] = temp;

    // Final AddRoundKey
    for (int c = 0; c < 4; ++c) {
        const uint32_t rk = ctx->roundKeys[ctx->rounds * 4 + c];
        state[0][c] ^= static_cast<uint8_t>((rk >> 24) & 0xFF);
        state[1][c] ^= static_cast<uint8_t>((rk >> 16) & 0xFF);
        state[2][c] ^= static_cast<uint8_t>((rk >> 8) & 0xFF);
        state[3][c] ^= static_cast<uint8_t>(rk & 0xFF);
    }

    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            out[r + 4 * c] = state[r][c];
        }
    }
}

void AesCtrCrypt(const AesContext* ctx, uint8_t counter[16],
                 const uint8_t* in, uint8_t* out, size_t len) {
    if (ctx == NULL || counter == NULL || in == NULL || out == NULL || len == 0) return;

    uint8_t keystream[16];
    size_t processed = 0;

    while (processed < len) {
        AesEncryptBlock(ctx, counter, keystream);

        // Increment 16-byte counter (little-endian 128-bit integer)
        for (int i = 0; i < 16; ++i) {
            if (++counter[i] != 0) {
                break;
            }
        }

        const size_t chunk = (std::min<size_t>)(len - processed, 16);
        for (size_t i = 0; i < chunk; ++i) {
            out[processed + i] = in[processed + i] ^ keystream[i];
        }
        processed += chunk;
    }
}

}  // namespace crypto
}  // namespace pzip
