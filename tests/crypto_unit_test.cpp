#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "crypto/aes.h"
#include "crypto/sha1.h"
#include "crypto/zip_crypto.h"

int main() {
    using namespace pzip::crypto;

    // 1. Test SHA-1 with RFC 3174 test vector 1: "abc"
    // Digest: a9993e36 4706816a ba3e2571 7850c26c 9cd0d89d
    {
        uint8_t digest[20];
        Sha1(reinterpret_cast<const uint8_t*>("abc"), 3, digest);
        const uint8_t expected[20] = {
            0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e,
            0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d
        };
        assert(std::memcmp(digest, expected, 20) == 0);
    }

    // 2. Test PBKDF2-HMAC-SHA1 with RFC 6070 test vector 1:
    // P = "password", S = "salt", c = 1, dkLen = 20
    // Expected: 0c60c80f 961f0e71 f3a9b524 af601206 2fe037a6
    {
        uint8_t dk[20];
        Pbkdf2HmacSha1(reinterpret_cast<const uint8_t*>("password"), 8,
                       reinterpret_cast<const uint8_t*>("salt"), 4, 1, dk, 20);
        const uint8_t expected[20] = {
            0x0c, 0x60, 0xc8, 0x0f, 0x96, 0x1f, 0x0e, 0x71, 0xf3, 0xa9,
            0xb5, 0x24, 0xaf, 0x60, 0x12, 0x06, 0x2f, 0xe0, 0x37, 0xa6
        };
        assert(std::memcmp(dk, expected, 20) == 0);
    }

    // 3. Test AES-256 block encryption NIST SP 800-38A
    // Key: 000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f
    // Plaintext: 00112233445566778899aabbccddeeff
    // Ciphertext: 8ea2b7ca516745bfeafc49904b496089
    {
        uint8_t key[32];
        for (int i = 0; i < 32; ++i) key[i] = static_cast<uint8_t>(i);
        uint8_t pt[16] = {
            0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
            0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff
        };
        uint8_t ct[16];
        AesContext ctx;
        assert(AesSetEncryptKey(&ctx, key, 32));
        AesEncryptBlock(&ctx, pt, ct);
        const uint8_t expected[16] = {
            0x8e, 0xa2, 0xb7, 0xca, 0x51, 0x67, 0x45, 0xbf,
            0xea, 0xfc, 0x49, 0x90, 0x4b, 0x49, 0x60, 0x89
        };
        assert(std::memcmp(ct, expected, 16) == 0);
    }

    // 4. Test WinZip AES-256 roundtrip & wrong password rejection
    {
        std::string password = "SecretPassword123!";
        std::string secretData = "Hello, this is a secret payload for WinZip AES-256 test! 1234567890";
        std::vector<uint8_t> encrypted;
        assert(WinZipAesEncrypt(password, 3, reinterpret_cast<const uint8_t*>(secretData.data()), secretData.size(), &encrypted));
        assert(encrypted.size() == 16 + 2 + secretData.size() + 10);

        // Correct password decrypt
        std::vector<uint8_t> decrypted;
        assert(WinZipAesDecrypt(password, 3, encrypted.data(), encrypted.size(), &decrypted));
        assert(decrypted.size() == secretData.size());
        assert(std::memcmp(decrypted.data(), secretData.data(), secretData.size()) == 0);

        // Wrong password decrypt
        std::vector<uint8_t> wrongDecrypted;
        assert(!WinZipAesDecrypt("WrongPassword", 3, encrypted.data(), encrypted.size(), &wrongDecrypted));

        // Corrupted ciphertext auth tag failure
        std::vector<uint8_t> corrupted = encrypted;
        corrupted[corrupted.size() - 5] ^= 0x55;
        assert(!WinZipAesDecrypt(password, 3, corrupted.data(), corrupted.size(), &wrongDecrypted));
    }

    // 5. Test WinZip AES-128 roundtrip
    {
        std::string password = "SecretPassword128!";
        std::string secretData = "AES-128 test payload";
        std::vector<uint8_t> encrypted;
        assert(WinZipAesEncrypt(password, 1, reinterpret_cast<const uint8_t*>(secretData.data()), secretData.size(), &encrypted));
        assert(encrypted.size() == 8 + 2 + secretData.size() + 10);

        std::vector<uint8_t> decrypted;
        assert(WinZipAesDecrypt(password, 1, encrypted.data(), encrypted.size(), &decrypted));
        assert(decrypted.size() == secretData.size());
        assert(std::memcmp(decrypted.data(), secretData.data(), secretData.size()) == 0);
        assert(!WinZipAesDecrypt("WrongPassword", 1, encrypted.data(), encrypted.size(), &decrypted));
    }

    // 6. Test ZipCrypto roundtrip & wrong password rejection
    {
        std::string password = "ZipCryptoPassword";
        std::string secretData = "Traditional PKWARE ZipCrypto test payload abcdefghijklmnopqrstuvwxyz";
        uint32_t crc32 = 0x12345678U;
        uint16_t dosTime = 0x4321U;

        std::vector<uint8_t> encrypted;
        assert(ZipCryptoEncrypt(password, crc32, reinterpret_cast<const uint8_t*>(secretData.data()), secretData.size(), &encrypted));
        assert(encrypted.size() == 12 + secretData.size());

        std::vector<uint8_t> decrypted;
        assert(ZipCryptoDecrypt(password, crc32, dosTime, false, encrypted.data(), encrypted.size(), &decrypted));
        assert(decrypted.size() == secretData.size());
        assert(std::memcmp(decrypted.data(), secretData.data(), secretData.size()) == 0);

        // Wrong password
        std::vector<uint8_t> wrongDecrypted;
        assert(!ZipCryptoDecrypt("WrongZipCryptoPass", crc32, dosTime, false, encrypted.data(), encrypted.size(), &wrongDecrypted));
    }

    std::printf("All crypto unit tests passed successfully!\n");
    return 0;
}
