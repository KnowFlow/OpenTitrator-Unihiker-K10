#include "auth_core.h"

namespace {
bool decodeHex(const char *text, uint8_t out[AuthSession::TokenBytes]) {
  if (text == nullptr) return false;
  for (size_t i = 0; i < AuthSession::TokenBytes * 2; ++i) {
    const char c = text[i];
    if (c == '\0') return false;
    uint8_t nibble = 0;
    if (c >= '0' && c <= '9') nibble = static_cast<uint8_t>(c - '0');
    else if (c >= 'a' && c <= 'f') nibble = static_cast<uint8_t>(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') nibble = static_cast<uint8_t>(c - 'A' + 10);
    else return false;
    if ((i & 1U) == 0) out[i / 2] = static_cast<uint8_t>(nibble << 4);
    else out[i / 2] = static_cast<uint8_t>(out[i / 2] | nibble);
  }
  return text[AuthSession::TokenBytes * 2] == '\0';
}

bool equalBytes(const uint8_t *left, const uint8_t *right, size_t count) {
  uint8_t difference = 0;
  for (size_t i = 0; i < count; ++i)
    difference = static_cast<uint8_t>(difference | (left[i] ^ right[i]));
  return difference == 0;
}
}

AuthManager::AuthManager(AuthCrypto &crypto) : crypto_(crypto) {}

void AuthManager::setAdministratorCredential(const AuthCredential &credential) {
  administrator_ = credential;
}
void AuthManager::setFactoryCredential(const AuthCredential &credential) {
  factory_ = credential;
}
bool AuthManager::hasAdministratorCredential() const { return administrator_.valid; }

bool AuthManager::validAdministratorPassword(const char *password, size_t bytes) {
  return password != nullptr && bytes >= 10 && bytes <= 64;
}

bool AuthManager::verify(const AuthCredential &credential, const char *password,
                         size_t bytes) {
  uint8_t derived[AuthCredential::HashBytes] = {};
  if (!credential.valid || password == nullptr || credential.iterations == 0 ||
      !crypto_.derive(password, bytes, credential.salt, AuthCredential::SaltBytes,
                      credential.iterations, derived)) return false;
  return equalBytes(derived, credential.hash, AuthCredential::HashBytes);
}

uint32_t AuthManager::nextSerial() {
  if (serial_ == UINT32_MAX) {
    uint8_t active[MaxSessions] = {};
    uint8_t count = 0;
    for (uint8_t i = 0; i < MaxSessions; ++i)
      if (sessions_[i].active) active[count++] = i;
    if (count == 2 && sessions_[active[1]].lastUsedSerial <
                          sessions_[active[0]].lastUsedSerial) {
      const uint8_t swap = active[0]; active[0] = active[1]; active[1] = swap;
    }
    for (uint8_t rank = 0; rank < count; ++rank)
      sessions_[active[rank]].lastUsedSerial = static_cast<uint32_t>(rank + 1U);
    serial_ = count;
  }
  ++serial_;
  return serial_;
}

AuthResult AuthManager::login(const char *password, size_t bytes, uint32_t now,
                              char tokenHex[33]) {
  if (!administrator_.valid) return AuthResult::Required;
  if (tokenHex == nullptr || !validAdministratorPassword(password, bytes))
    return AuthResult::InvalidPassword;
  if (loginLocked_) {
    if (static_cast<uint32_t>(now - loginLockedAt_) < LoginLockMs)
      return AuthResult::RateLimited;
    loginLocked_ = false;
    loginFailures_ = 0;
  }
  if (!verify(administrator_, password, bytes)) {
    ++loginFailures_;
    if (loginFailures_ >= 5) {
      loginLocked_ = true;
      loginLockedAt_ = now;
      return AuthResult::RateLimited;
    }
    return AuthResult::Failed;
  }
  loginFailures_ = 0;
  uint8_t selected = MaxSessions;
  for (uint8_t i = 0; i < MaxSessions; ++i)
    if (!sessions_[i].active) { selected = i; break; }
  if (selected == MaxSessions) {
    selected = 0;
    for (uint8_t i = 1; i < MaxSessions; ++i)
      if (sessions_[i].lastUsedSerial < sessions_[selected].lastUsedSerial)
        selected = i;
  }
  AuthSession &session = sessions_[selected];
  crypto_.randomBytes(session.token, AuthSession::TokenBytes);
  session.lastWriteMs = now;
  session.lastUsedSerial = nextSerial();
  session.active = true;
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < AuthSession::TokenBytes; ++i) {
    tokenHex[i * 2] = digits[session.token[i] >> 4];
    tokenHex[i * 2 + 1] = digits[session.token[i] & 0x0f];
  }
  tokenHex[32] = '\0';
  return AuthResult::Ok;
}

AuthResult AuthManager::validateSession(const char *tokenHex, uint32_t now,
                                        uint8_t &slot) {
  uint8_t token[AuthSession::TokenBytes] = {};
  if (!decodeHex(tokenHex, token)) return AuthResult::Required;
  for (uint8_t i = 0; i < MaxSessions; ++i) {
    AuthSession &session = sessions_[i];
    if (session.active && equalBytes(token, session.token, AuthSession::TokenBytes)) {
      if (static_cast<uint32_t>(now - session.lastWriteMs) >= SessionIdleMs) {
        session.active = false;
        return AuthResult::Expired;
      }
      session.lastUsedSerial = nextSerial();
      slot = i;
      return AuthResult::Ok;
    }
  }
  return AuthResult::Required;
}

void AuthManager::recordSuccessfulWrite(uint8_t slot, uint32_t now) {
  if (slot >= MaxSessions || !sessions_[slot].active) return;
  sessions_[slot].lastWriteMs = now;
  sessions_[slot].lastUsedSerial = nextSerial();
}

AuthResult AuthManager::logout(const char *tokenHex) {
  uint8_t token[AuthSession::TokenBytes] = {};
  if (!decodeHex(tokenHex, token)) return AuthResult::Required;
  for (uint8_t i = 0; i < MaxSessions; ++i) {
    if (sessions_[i].active && equalBytes(token, sessions_[i].token,
                                          AuthSession::TokenBytes)) {
      sessions_[i].active = false;
      return AuthResult::Ok;
    }
  }
  return AuthResult::Required;
}

void AuthManager::clearSessions() {
  for (uint8_t i = 0; i < MaxSessions; ++i) sessions_[i] = AuthSession{};
}

AuthResult AuthManager::recover(const char *factoryPassword, size_t factoryBytes,
                                const char *newPassword, size_t newBytes,
                                uint32_t now, SaveCredentialFn save,
                                void *saveContext) {
  if (recoveryLocked_) {
    if (static_cast<uint32_t>(now - recoveryLockedAt_) < RecoveryLockMs)
      return AuthResult::RateLimited;
    recoveryLocked_ = false;
    recoveryFailures_ = 0;
  }
  if (!verify(factory_, factoryPassword, factoryBytes)) {
    ++recoveryFailures_;
    if (recoveryFailures_ >= 3) {
      recoveryLocked_ = true;
      recoveryLockedAt_ = now;
      return AuthResult::RateLimited;
    }
    return AuthResult::Failed;
  }
  if (!validAdministratorPassword(newPassword, newBytes))
    return AuthResult::InvalidPassword;
  if (save == nullptr) return AuthResult::StorageError;
  AuthCredential replacement;
  replacement.iterations = DefaultIterations;
  crypto_.randomBytes(replacement.salt, AuthCredential::SaltBytes);
  if (!crypto_.derive(newPassword, newBytes, replacement.salt,
                      AuthCredential::SaltBytes, replacement.iterations,
                      replacement.hash)) return AuthResult::Failed;
  replacement.valid = true;
  if (!save(saveContext, replacement)) return AuthResult::StorageError;
  administrator_ = replacement;
  loginFailures_ = 0;
  recoveryFailures_ = 0;
  loginLocked_ = false;
  recoveryLocked_ = false;
  clearSessions();
  return AuthResult::Ok;
}
