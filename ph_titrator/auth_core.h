#pragma once
#include <stddef.h>
#include <stdint.h>

struct AuthCredential {
  static constexpr size_t SaltBytes = 16;
  static constexpr size_t HashBytes = 32;
  uint8_t version = 1;
  uint32_t iterations = 0;
  uint8_t salt[SaltBytes] = {};
  uint8_t hash[HashBytes] = {};
  bool valid = false;
};

class AuthCrypto {
public:
  virtual ~AuthCrypto() = default;
  virtual bool derive(const char *, size_t, const uint8_t *, size_t, uint32_t,
                      uint8_t out[AuthCredential::HashBytes]) = 0;
  virtual void randomBytes(uint8_t *out, size_t count) = 0;
};

enum class AuthResult : uint8_t {
  Ok, Required, Expired, Failed, RateLimited, InvalidPassword, StorageError
};

struct AuthSession {
  static constexpr size_t TokenBytes = 16;
  uint8_t token[TokenBytes] = {};
  uint32_t lastWriteMs = 0;
  uint32_t lastUsedSerial = 0;
  bool active = false;
};

using SaveCredentialFn = bool (*)(void *, const AuthCredential &);

class AuthManager {
  friend struct AuthManagerTestAccess;
public:
  static constexpr uint32_t SessionIdleMs = 30UL * 60UL * 1000UL;
  static constexpr uint32_t LoginLockMs = 60UL * 1000UL;
  static constexpr uint32_t RecoveryLockMs = 5UL * 60UL * 1000UL;
  static constexpr uint8_t MaxSessions = 2;
  static constexpr uint32_t DefaultIterations = 120000;

  explicit AuthManager(AuthCrypto &crypto);
  void setAdministratorCredential(const AuthCredential &credential);
  void setFactoryCredential(const AuthCredential &credential);
  bool hasAdministratorCredential() const;
  static bool validAdministratorPassword(const char *password, size_t bytes);
  AuthResult authenticateAdministrator(const char *password, size_t bytes,
                                       uint32_t now);
  AuthResult login(const char *password, size_t bytes, uint32_t now,
                   char tokenHex[33]);
  AuthResult validateSession(const char *tokenHex, uint32_t now, uint8_t &slot);
  void recordSuccessfulWrite(uint8_t slot, uint32_t now);
  AuthResult logout(const char *tokenHex);
  void clearSessions();
  AuthResult recover(const char *factoryPassword, size_t factoryBytes,
                     const char *newPassword, size_t newBytes, uint32_t now,
                     SaveCredentialFn save, void *saveContext);

private:
  bool verify(const AuthCredential &credential, const char *password,
              size_t bytes);
  uint32_t nextSerial();

  AuthCrypto &crypto_;
  AuthCredential administrator_;
  AuthCredential factory_;
  AuthSession sessions_[MaxSessions] = {};
  uint32_t serial_ = 0;
  uint8_t loginFailures_ = 0;
  uint8_t recoveryFailures_ = 0;
  uint32_t loginLockedAt_ = 0;
  uint32_t recoveryLockedAt_ = 0;
  bool loginLocked_ = false;
  bool recoveryLocked_ = false;
};
