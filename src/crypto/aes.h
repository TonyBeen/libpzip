#ifndef PZIP_CRYPTO_AES_H_
#define PZIP_CRYPTO_AES_H_

#include <stddef.h>
#include <stdint.h>

namespace pzip {
namespace crypto {

struct AesContext {
    uint32_t roundKeys[60];
    int rounds;
};

// Initializes AES encryption key. keyLen must be 16 (AES-128) or 32 (AES-256).
bool AesSetEncryptKey(AesContext* ctx, const uint8_t* key, size_t keyLen);

// Encrypts a single 16-byte block.
void AesEncryptBlock(const AesContext* ctx, const uint8_t in[16], uint8_t out[16]);

// WinZip AES CTR mode encryption/decryption:
// Counter is a 16-byte block incremented as a little-endian 128-bit integer.
void AesCtrCrypt(const AesContext* ctx, uint8_t counter[16],
                 const uint8_t* in, uint8_t* out, size_t len);

}  // namespace crypto
}  // namespace pzip

#endif  // PZIP_CRYPTO_AES_H_
