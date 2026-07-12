#include "auth_crypto_esp32.h"
#include <esp_system.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>

bool Esp32AuthCrypto::derive(const char *password, size_t passwordBytes,
                             const uint8_t *salt, size_t saltBytes,
                             uint32_t iterations,
                             uint8_t out[AuthCredential::HashBytes]) {
  if (!password || !salt || !out || iterations == 0) return false;
  mbedtls_md_context_t context;
  mbedtls_md_init(&context);
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  int result = info ? mbedtls_md_setup(&context, info, 1) : MBEDTLS_ERR_MD_BAD_INPUT_DATA;
  if (result == 0)
    result = mbedtls_pkcs5_pbkdf2_hmac(&context,
        reinterpret_cast<const unsigned char *>(password), passwordBytes,
        salt, saltBytes, iterations, AuthCredential::HashBytes, out);
  mbedtls_md_free(&context);
  return result == 0;
}

void Esp32AuthCrypto::randomBytes(uint8_t *out, size_t count) {
  if (out && count) esp_fill_random(out, count);
}
