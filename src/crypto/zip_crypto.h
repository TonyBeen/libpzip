#ifndef PZIP_CRYPTO_ZIP_CRYPTO_H_
#define PZIP_CRYPTO_ZIP_CRYPTO_H_

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace pzip {
namespace crypto {

void GetRandomBytes(uint8_t* buf, size_t len);

bool ConstantTimeEquals(const uint8_t* a, const uint8_t* b, size_t len);

bool ZipCryptoEncrypt(const std::string& password, uint32_t crc32,
                      const uint8_t* in, size_t inLen,
                      std::vector<uint8_t>* outEncrypted);

bool ZipCryptoDecrypt(const std::string& password, uint32_t crc32, uint16_t dosTime,
                      bool hasDataDescriptor,
                      const uint8_t* in, size_t inLen,
                      std::vector<uint8_t>* outDecrypted);

bool WinZipAesEncrypt(const std::string& password, int strength /* 1=128, 3=256 */,
                      const uint8_t* in, size_t inLen,
                      std::vector<uint8_t>* outEncrypted);

bool WinZipAesDecrypt(const std::string& password, int strength /* 1=128, 3=256 */,
                      const uint8_t* in, size_t inLen,
                      std::vector<uint8_t>* outDecrypted);

}  // namespace crypto
}  // namespace pzip

#endif  // PZIP_CRYPTO_ZIP_CRYPTO_H_
