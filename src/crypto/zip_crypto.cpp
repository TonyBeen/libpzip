#include "crypto/zip_crypto.h"

#include <chrono>
#include <cstring>
#include <random>

#include "crypto/aes.h"
#include "crypto/sha1.h"

namespace pzip {
namespace crypto {

namespace {

uint32_t TableCrc32Byte(uint32_t crc, uint8_t b) {
    static uint32_t table[256];
    static bool initialized = false;
    if (!initialized) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        initialized = true;
    }
    return (crc >> 8) ^ table[(crc ^ b) & 0xFF];
}

void ZipCryptoUpdate(uint32_t keys[3], uint8_t b) {
    keys[0] = TableCrc32Byte(keys[0], b);
    keys[1] = (keys[1] + (keys[0] & 0xFF)) * 134775813U + 1U;
    keys[2] = TableCrc32Byte(keys[2], static_cast<uint8_t>((keys[1] >> 24) & 0xFF));
}

uint8_t ZipCryptoDecryptByte(const uint32_t keys[3]) {
    const uint16_t temp = static_cast<uint16_t>((keys[2] & 0xFFFF) | 2);
    return static_cast<uint8_t>(((temp * (temp ^ 1)) >> 8) & 0xFF);
}

void ZipCryptoInit(uint32_t keys[3], const std::string& password) {
    keys[0] = 0x12345678U;
    keys[1] = 0x23456789U;
    keys[2] = 0x34567890U;
    for (size_t i = 0; i < password.size(); ++i) {
        ZipCryptoUpdate(keys, static_cast<uint8_t>(password[i]));
    }
}

}  // namespace

void GetRandomBytes(uint8_t* buf, size_t len) {
    if (buf == NULL || len == 0) return;

    static thread_local std::random_device rd;
    const uint64_t timeSeed = static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    static thread_local std::mt19937_64 gen(rd() ^ timeSeed);

    for (size_t i = 0; i < len; ++i) {
        buf[i] = static_cast<uint8_t>(gen() & 0xFF);
    }
}

bool ConstantTimeEquals(const uint8_t* a, const uint8_t* b, size_t len) {
    if (a == NULL || b == NULL) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i) {
        diff |= (a[i] ^ b[i]);
    }
    return diff == 0;
}

bool ZipCryptoEncrypt(const std::string& password, uint32_t crc32,
                      const uint8_t* in, size_t inLen,
                      std::vector<uint8_t>* outEncrypted) {
    if (outEncrypted == NULL) return false;

    uint8_t header[12];
    GetRandomBytes(header, 10);
    header[10] = static_cast<uint8_t>((crc32 >> 16) & 0xFF);
    header[11] = static_cast<uint8_t>((crc32 >> 24) & 0xFF);

    uint32_t keys[3];
    ZipCryptoInit(keys, password);

    outEncrypted->resize(12 + inLen);
    for (int i = 0; i < 12; ++i) {
        const uint8_t p = header[i];
        (*outEncrypted)[i] = p ^ ZipCryptoDecryptByte(keys);
        ZipCryptoUpdate(keys, p);
    }

    for (size_t i = 0; i < inLen; ++i) {
        const uint8_t p = in[i];
        (*outEncrypted)[12 + i] = p ^ ZipCryptoDecryptByte(keys);
        ZipCryptoUpdate(keys, p);
    }
    return true;
}

bool ZipCryptoDecrypt(const std::string& password, uint32_t crc32, uint16_t dosTime,
                      bool hasDataDescriptor,
                      const uint8_t* in, size_t inLen,
                      std::vector<uint8_t>* outDecrypted) {
    if (in == NULL || outDecrypted == NULL || inLen < 12) return false;

    uint32_t keys[3];
    ZipCryptoInit(keys, password);

    uint8_t header[12];
    for (int i = 0; i < 12; ++i) {
        const uint8_t c = in[i];
        const uint8_t p = c ^ ZipCryptoDecryptByte(keys);
        ZipCryptoUpdate(keys, p);
        header[i] = p;
    }

    if (hasDataDescriptor) {
        if (header[11] != static_cast<uint8_t>((dosTime >> 8) & 0xFF)) {
            return false;
        }
    } else {
        if (header[11] != static_cast<uint8_t>((crc32 >> 24) & 0xFF) ||
            header[10] != static_cast<uint8_t>((crc32 >> 16) & 0xFF)) {
            return false;
        }
    }

    outDecrypted->resize(inLen - 12);
    for (size_t i = 0; i < inLen - 12; ++i) {
        const uint8_t c = in[12 + i];
        const uint8_t p = c ^ ZipCryptoDecryptByte(keys);
        ZipCryptoUpdate(keys, p);
        (*outDecrypted)[i] = p;
    }
    return true;
}

bool WinZipAesEncrypt(const std::string& password, int strength,
                      const uint8_t* in, size_t inLen,
                      std::vector<uint8_t>* outEncrypted) {
    if (outEncrypted == NULL) return false;

    size_t saltLen = 16;
    size_t keyLen = 32;
    if (strength == 1) {
        saltLen = 8;
        keyLen = 16;
    } else if (strength == 3) {
        saltLen = 16;
        keyLen = 32;
    } else {
        return false;
    }

    std::vector<uint8_t> salt(saltLen);
    GetRandomBytes(&salt[0], saltLen);

    const size_t dkLen = 2 * keyLen + 2;
    std::vector<uint8_t> keyMaterial(dkLen);
    Pbkdf2HmacSha1(reinterpret_cast<const uint8_t*>(password.data()), password.size(),
                   &salt[0], saltLen, 1000, &keyMaterial[0], dkLen);

    const uint8_t* aesKey = &keyMaterial[0];
    const uint8_t* hmacKey = &keyMaterial[keyLen];
    const uint8_t* pv = &keyMaterial[2 * keyLen];

    // Layout: [salt (saltLen)] + [pv (2)] + [ciphertext (inLen)] + [auth code (10)]
    const size_t totalLen = saltLen + 2 + inLen + 10;
    outEncrypted->resize(totalLen);

    // 1. Copy salt
    std::memcpy(&(*outEncrypted)[0], &salt[0], saltLen);

    // 2. Copy password verification value
    (*outEncrypted)[saltLen + 0] = pv[0];
    (*outEncrypted)[saltLen + 1] = pv[1];

    // 3. Encrypt payload in CTR mode
    uint8_t* cipherDst = &(*outEncrypted)[saltLen + 2];
    AesContext aes;
    if (!AesSetEncryptKey(&aes, aesKey, keyLen)) {
        return false;
    }
    uint8_t counter[16];
    std::memset(counter, 0, sizeof(counter));
    counter[0] = 1;
    if (inLen > 0 && in != NULL) {
        AesCtrCrypt(&aes, counter, in, cipherDst, inLen);
    }

    // 4. Compute HMAC-SHA1 over ciphertext and take first 10 bytes
    uint8_t hmac[20];
    HmacSha1(hmacKey, keyLen, cipherDst, inLen, hmac);
    std::memcpy(&(*outEncrypted)[saltLen + 2 + inLen], hmac, 10);

    return true;
}

bool WinZipAesDecrypt(const std::string& password, int strength,
                      const uint8_t* in, size_t inLen,
                      std::vector<uint8_t>* outDecrypted) {
    if (outDecrypted == NULL || in == NULL) return false;

    size_t saltLen = 16;
    size_t keyLen = 32;
    if (strength == 1) {
        saltLen = 8;
        keyLen = 16;
    } else if (strength == 3) {
        saltLen = 16;
        keyLen = 32;
    } else {
        return false;
    }

    if (inLen < saltLen + 2 + 10) {
        return false;
    }

    const uint8_t* salt = in;
    const uint8_t* pvIn = in + saltLen;
    const size_t cipherLen = inLen - saltLen - 2 - 10;
    const uint8_t* cipherPtr = in + saltLen + 2;
    const uint8_t* storedAuthCode = in + saltLen + 2 + cipherLen;

    const size_t dkLen = 2 * keyLen + 2;
    std::vector<uint8_t> keyMaterial(dkLen);
    Pbkdf2HmacSha1(reinterpret_cast<const uint8_t*>(password.data()), password.size(),
                   salt, saltLen, 1000, &keyMaterial[0], dkLen);

    const uint8_t* aesKey = &keyMaterial[0];
    const uint8_t* hmacKey = &keyMaterial[keyLen];
    const uint8_t* pv = &keyMaterial[2 * keyLen];

    // Verify PV (Password Verification)
    if (pvIn[0] != pv[0] || pvIn[1] != pv[1]) {
        return false;
    }

    // Verify HMAC-SHA1 auth code
    uint8_t hmac[20];
    HmacSha1(hmacKey, keyLen, cipherPtr, cipherLen, hmac);
    if (!ConstantTimeEquals(hmac, storedAuthCode, 10)) {
        return false;
    }

    // Decrypt ciphertext
    outDecrypted->resize(cipherLen);
    AesContext aes;
    if (!AesSetEncryptKey(&aes, aesKey, keyLen)) {
        return false;
    }
    uint8_t counter[16];
    std::memset(counter, 0, sizeof(counter));
    counter[0] = 1;
    if (cipherLen > 0) {
        AesCtrCrypt(&aes, counter, cipherPtr, &(*outDecrypted)[0], cipherLen);
    }
    return true;
}

}  // namespace crypto
}  // namespace pzip
