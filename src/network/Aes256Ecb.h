#pragma once

#include <cstddef>
#include <cstdint>

// Minimal AES-256 block decryption (FIPS-197 inverse cipher) for ECB streams.
// Portable C++ so the device and the desktop simulator share one code path;
// the round keys (240 bytes) live inside the object, no heap.
class Aes256EcbDecryptor {
 public:
  static constexpr size_t BLOCK = 16;
  static constexpr size_t KEY_SIZE = 32;

  explicit Aes256EcbDecryptor(const uint8_t key[KEY_SIZE]);

  // Decrypts one 16-byte block in place.
  void decryptBlock(uint8_t block[BLOCK]) const;

 private:
  static constexpr int ROUNDS = 14;
  uint8_t roundKeys[BLOCK * (ROUNDS + 1)];
};

// Feeds an AES-ECB ciphertext stream in arbitrary chunks and emits plaintext
// block by block. PKCS7 padding is left in the output of the final block;
// JSON consumers ignore the trailing control bytes.
class AesEcbStream {
 public:
  using PlainCallback = void (*)(const uint8_t* data, size_t len, void* ctx);

  AesEcbStream(const Aes256EcbDecryptor& aes, PlainCallback onPlain, void* ctx)
      : aes(aes), onPlain(onPlain), ctx(ctx) {}

  void feed(const uint8_t* data, size_t len);
  size_t bytesIn() const { return total; }

 private:
  const Aes256EcbDecryptor& aes;
  PlainCallback onPlain;
  void* ctx;
  uint8_t pending[Aes256EcbDecryptor::BLOCK] = {};
  size_t pendingLen = 0;
  size_t total = 0;
};
