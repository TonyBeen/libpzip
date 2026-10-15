#ifndef PZIP_CRYPTO_SHA1_H_
#define PZIP_CRYPTO_SHA1_H_

#include <stddef.h>
#include <stdint.h>

namespace pzip {
namespace crypto {

struct Sha1Context {
    uint32_t state[5];
    uint64_t count;
    uint8_t buffer[64];
};

void Sha1Init(Sha1Context* ctx);
void Sha1Update(Sha1Context* ctx, const uint8_t* data, size_t len);
void Sha1Final(Sha1Context* ctx, uint8_t digest[20]);
void Sha1(const uint8_t* data, size_t len, uint8_t digest[20]);

void HmacSha1(const uint8_t* key, size_t keyLen,
              const uint8_t* data, size_t dataLen,
              uint8_t out[20]);

void Pbkdf2HmacSha1(const uint8_t* password, size_t passLen,
                    const uint8_t* salt, size_t saltLen,
                    uint32_t iterations,
                    uint8_t* outKey, size_t keyLen);

}  // namespace crypto
}  // namespace pzip

#endif  // PZIP_CRYPTO_SHA1_H_
