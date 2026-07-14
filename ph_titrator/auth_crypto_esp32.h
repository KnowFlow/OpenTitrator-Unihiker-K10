#pragma once
#include "auth_core.h"

class Esp32AuthCrypto final : public AuthCrypto {
public:
  bool derive(const char *password, size_t passwordBytes, const uint8_t *salt,
              size_t saltBytes, uint32_t iterations,
              uint8_t out[AuthCredential::HashBytes]) override;
  void randomBytes(uint8_t *out, size_t count) override;
};
