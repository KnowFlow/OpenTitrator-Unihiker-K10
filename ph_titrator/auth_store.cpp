#include "auth_store.h"
#include <Preferences.h>
#include <cstring>

#if __has_include("factory_auth.generated.h")
#include "factory_auth.generated.h"
#else
#error "Missing factory_auth.generated.h: run scripts/generate_factory_auth.py before device builds"
static constexpr uint8_t FACTORY_AUTH_VERSION = 0;
static constexpr uint32_t FACTORY_AUTH_ITERATIONS = 0;
static constexpr uint8_t FACTORY_AUTH_SALT[AuthCredential::SaltBytes] = {};
static constexpr uint8_t FACTORY_AUTH_HASH[AuthCredential::HashBytes] = {};
#endif

bool AuthStore::loadAdministrator(AuthCredential &out) {
  Preferences prefs;
  if (!prefs.begin("auth", true)) return false;
  AuthCredential value;
  value.version = prefs.getUChar("admin_ver", 0);
  value.iterations = prefs.getUInt("admin_iter", 0);
  const size_t salt = prefs.getBytesLength("admin_salt");
  const size_t hash = prefs.getBytesLength("admin_hash");
  bool ok = value.version == 1 && value.iterations > 0 &&
            salt == AuthCredential::SaltBytes && hash == AuthCredential::HashBytes;
  if (ok) ok = prefs.getBytes("admin_salt", value.salt, sizeof value.salt) == sizeof value.salt &&
               prefs.getBytes("admin_hash", value.hash, sizeof value.hash) == sizeof value.hash;
  prefs.end();
  if (!ok) return false;
  value.valid = true; out = value; return true;
}

bool AuthStore::saveAdministrator(const AuthCredential &value) {
  if (!value.valid || value.version != 1 || value.iterations == 0) return false;
  Preferences prefs;
  if (!prefs.begin("auth", false)) return false;
  bool ok = prefs.putUChar("admin_ver", value.version) == sizeof(uint8_t) &&
            prefs.putUInt("admin_iter", value.iterations) == sizeof(uint32_t) &&
            prefs.putBytes("admin_salt", value.salt, sizeof value.salt) == sizeof value.salt &&
            prefs.putBytes("admin_hash", value.hash, sizeof value.hash) == sizeof value.hash;
  prefs.end(); return ok;
}

bool AuthStore::loadFactory(AuthCredential &out) {
  static_assert(sizeof FACTORY_AUTH_SALT == AuthCredential::SaltBytes, "factory salt length");
  static_assert(sizeof FACTORY_AUTH_HASH == AuthCredential::HashBytes, "factory hash length");
  if (FACTORY_AUTH_VERSION != 1 || FACTORY_AUTH_ITERATIONS == 0) return false;
  out = AuthCredential{}; out.version = FACTORY_AUTH_VERSION;
  out.iterations = FACTORY_AUTH_ITERATIONS;
  memcpy(out.salt, FACTORY_AUTH_SALT, sizeof out.salt);
  memcpy(out.hash, FACTORY_AUTH_HASH, sizeof out.hash);
  out.valid = true; return true;
}
