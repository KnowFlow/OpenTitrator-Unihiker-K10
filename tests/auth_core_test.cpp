#include "../ph_titrator/auth_core.h"

struct AuthManagerTestAccess {
  static void forceSerial(AuthManager &auth, uint32_t serial) { auth.serial_ = serial; }
};
#include "../ph_titrator/command_admission.h"

#include <cstdio>
#include <cstring>
#include <stdint.h>

struct FakeCrypto : AuthCrypto {
  uint8_t nextRandom = 1;
  bool derive(const char *password, size_t passwordBytes, const uint8_t *salt,
              size_t saltBytes, uint32_t iterations,
              uint8_t out[AuthCredential::HashBytes]) override {
    if (password == nullptr || salt == nullptr || iterations == 0) return false;
    uint32_t state = iterations;
    for (size_t i = 0; i < passwordBytes; ++i)
      state = (state * 16777619U) ^ static_cast<uint8_t>(password[i]);
    for (size_t i = 0; i < saltBytes; ++i) state = (state * 16777619U) ^ salt[i];
    for (size_t i = 0; i < AuthCredential::HashBytes; ++i) {
      state = state * 1664525U + 1013904223U;
      out[i] = static_cast<uint8_t>(state >> 24);
    }
    return true;
  }
  void randomBytes(uint8_t *out, size_t count) override {
    for (size_t i = 0; i < count; ++i) out[i] = nextRandom++;
  }
};

static bool saveAdmin(void *context, const AuthCredential &credential) {
  *static_cast<AuthCredential *>(context) = credential;
  return true;
}
static bool failSave(void *, const AuthCredential &) { return false; }

static int failures = 0;
#define CHECK(expression) do { if (!(expression)) { \
  std::printf("FAIL line %d: %s\n", __LINE__, #expression); ++failures; } } while (0)

static AuthCredential credential(FakeCrypto &crypto, const char *password) {
  AuthCredential result;
  result.iterations = AuthManager::DefaultIterations;
  for (size_t i = 0; i < AuthCredential::SaltBytes; ++i)
    result.salt[i] = static_cast<uint8_t>(0xA0U + i);
  result.valid = crypto.derive(password, std::strlen(password), result.salt,
                               AuthCredential::SaltBytes, result.iterations,
                               result.hash);
  return result;
}

static void testPasswordLimitsAndVerification() {
  CHECK(!AuthManager::validAdministratorPassword("123456789", 9));
  CHECK(AuthManager::validAdministratorPassword("1234567890", 10));
  char longPassword[66] = {};
  std::memset(longPassword, 'x', 65);
  CHECK(AuthManager::validAdministratorPassword(longPassword, 64));
  CHECK(!AuthManager::validAdministratorPassword(longPassword, 65));
  CHECK(!AuthManager::validAdministratorPassword(nullptr, 10));

  FakeCrypto crypto;
  AuthManager auth(crypto);
  CHECK(!auth.hasAdministratorCredential());
  auth.setAdministratorCredential(credential(crypto, "correct-password"));
  CHECK(auth.hasAdministratorCredential());
  char token[33] = {};
  CHECK(auth.login("wrong-password", 14, 10, token) == AuthResult::Failed);
  CHECK(auth.login("correct-password", 16, 11, token) == AuthResult::Ok);
  CHECK(std::strlen(token) == 32);
}

static void testLoginLockout() {
  FakeCrypto crypto;
  AuthManager auth(crypto);
  auth.setAdministratorCredential(credential(crypto, "correct-password"));
  char token[33] = {};
  for (uint32_t i = 0; i < 4; ++i)
    CHECK(auth.login("wrong-password", 14, 100 + i, token) == AuthResult::Failed);
  CHECK(auth.login("wrong-password", 14, 104, token) == AuthResult::RateLimited);
  CHECK(auth.login("correct-password", 16, 104 + AuthManager::LoginLockMs - 1,
                   token) == AuthResult::RateLimited);
  CHECK(auth.login("correct-password", 16, 104 + AuthManager::LoginLockMs,
                   token) == AuthResult::Ok);
}

static void testDirectAdministratorAuthentication() {
  FakeCrypto crypto;
  AuthManager auth(crypto);
  auth.setAdministratorCredential(credential(crypto, "correct-password"));
  CHECK(auth.authenticateAdministrator("wrong-password", 14, 10) == AuthResult::Failed);
  CHECK(auth.authenticateAdministrator("correct-password", 16, 11) == AuthResult::Ok);
  uint8_t slot = 0;
  CHECK(auth.validateSession("00000000000000000000000000000000", 12, slot) ==
        AuthResult::Required);
}

static void testSessionsAndExpiry() {
  FakeCrypto crypto;
  AuthManager auth(crypto);
  auth.setAdministratorCredential(credential(crypto, "correct-password"));
  char first[33] = {}, second[33] = {}, third[33] = {};
  CHECK(auth.login("correct-password", 16, 100, first) == AuthResult::Ok);
  CHECK(auth.login("correct-password", 16, 101, second) == AuthResult::Ok);
  uint8_t slot = 99;
  CHECK(auth.validateSession(first, 200, slot) == AuthResult::Ok); // first is MRU
  CHECK(auth.login("correct-password", 16, 300, third) == AuthResult::Ok);
  CHECK(auth.validateSession(second, 301, slot) == AuthResult::Required);
  CHECK(auth.validateSession(first, 301, slot) == AuthResult::Ok);
  CHECK(auth.logout(first) == AuthResult::Ok);
  CHECK(auth.logout(first) == AuthResult::Required);
  CHECK(auth.validateSession(third, 302, slot) == AuthResult::Ok);
  auth.clearSessions();
  CHECK(auth.validateSession(third, 303, slot) == AuthResult::Required);

  CHECK(auth.login("correct-password", 16, UINT32_MAX - 1000U, first) == AuthResult::Ok);
  CHECK(auth.validateSession(first, 500U, slot) == AuthResult::Ok);
  CHECK(auth.validateSession(first, AuthManager::SessionIdleMs - 999U, slot) == AuthResult::Expired);

  CHECK(auth.login("correct-password", 16, 1000, first) == AuthResult::Ok);
  CHECK(auth.validateSession(first, 1000 + AuthManager::SessionIdleMs - 1, slot) == AuthResult::Ok);
  CHECK(auth.validateSession(first, 1000 + AuthManager::SessionIdleMs, slot) == AuthResult::Expired);
  CHECK(auth.login("correct-password", 16, 2000, first) == AuthResult::Ok);
  CHECK(auth.validateSession(first, 2500, slot) == AuthResult::Ok);
  auth.recordSuccessfulWrite(slot, 2500);
  CHECK(auth.validateSession(first, 2500 + AuthManager::SessionIdleMs - 1, slot) == AuthResult::Ok);
}

static void testMalformedTokens() {
  FakeCrypto crypto;
  AuthManager auth(crypto);
  uint8_t slot = 0;
  CHECK(auth.validateSession(nullptr, 0, slot) == AuthResult::Required);
  CHECK(auth.validateSession("abc", 0, slot) == AuthResult::Required);
  CHECK(auth.validateSession("zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz", 0, slot) == AuthResult::Required);
}

static void testRecovery() {
  FakeCrypto crypto;
  AuthManager auth(crypto);
  auth.setAdministratorCredential(credential(crypto, "old-password"));
  auth.setFactoryCredential(credential(crypto, "factory-password"));
  AuthCredential saved;
  for (uint32_t i = 0; i < 2; ++i)
    CHECK(auth.recover("wrong-factory", 13, "new-password", 12, 100 + i,
                       saveAdmin, &saved) == AuthResult::Failed);
  CHECK(auth.recover("wrong-factory", 13, "new-password", 12, 102,
                     saveAdmin, &saved) == AuthResult::RateLimited);
  CHECK(auth.recover("factory-password", 16, "new-password", 12,
                     102 + AuthManager::RecoveryLockMs - 1, saveAdmin,
                     &saved) == AuthResult::RateLimited);
  CHECK(auth.recover("factory-password", 16, "short", 5,
                     102 + AuthManager::RecoveryLockMs, saveAdmin,
                     &saved) == AuthResult::InvalidPassword);
  CHECK(auth.recover("factory-password", 16, "new-password", 12,
                     102 + AuthManager::RecoveryLockMs, failSave,
                     nullptr) == AuthResult::StorageError);

  char oldToken[33] = {};
  CHECK(auth.login("old-password", 12, 400000, oldToken) == AuthResult::Ok);
  CHECK(auth.recover("factory-password", 16, "new-password", 12, 400001,
                     saveAdmin, &saved) == AuthResult::Ok);
  uint8_t slot = 0;
  CHECK(saved.valid && saved.iterations == AuthManager::DefaultIterations);
  CHECK(auth.validateSession(oldToken, 400002, slot) == AuthResult::Required);
  char newToken[33] = {};
  CHECK(auth.login("new-password", 12, 400003, newToken) == AuthResult::Ok);
  CHECK(auth.login("old-password", 12, 400004, newToken) == AuthResult::Failed);
}

static void testCommandAdmissionPolicy() {
  const AdmissionContext idle = {true, false, false, false, false};
  const AdmissionContext unauthenticated = {false, false, false, false, false};
  const AdmissionContext otaLocked = {true, true, false, false, false};
  const AdmissionContext otaInProgress = {true, false, true, false, false};
  const AdmissionContext active = {true, false, false, true, false};
  const AdmissionContext calibrating = {true, false, false, false, true};

  const WebCommand authenticatedCommands[] = {
      WebCommand::Start, WebCommand::StartExisting, WebCommand::Pause,
      WebCommand::Reset, WebCommand::Tare, WebCommand::EnterReady,
      WebCommand::CalibratePumps, WebCommand::ResetSignalFilter,
      WebCommand::ManualTitrant, WebCommand::ManualSample,
      WebCommand::ManualSweep, WebCommand::ManualStop,
      WebCommand::SaveMethodSettings, WebCommand::SaveCalibration,
      WebCommand::SaveWifi, WebCommand::OtaUpload, WebCommand::Logout};
  for (size_t i = 0; i < sizeof(authenticatedCommands) /
                              sizeof(authenticatedCommands[0]); ++i) {
    CHECK(admitWebCommand(authenticatedCommands[i], unauthenticated) ==
          AdmissionResult::AuthenticationRequired);
    CHECK(admitWebCommand(authenticatedCommands[i], idle) ==
          AdmissionResult::Allowed);
  }

  const WebCommand otaBlockedCommands[] = {
      WebCommand::Start, WebCommand::StartExisting, WebCommand::Pause,
      WebCommand::Tare, WebCommand::EnterReady, WebCommand::CalibratePumps,
      WebCommand::ResetSignalFilter, WebCommand::ManualTitrant,
      WebCommand::ManualSample, WebCommand::ManualSweep,
      WebCommand::ManualStop, WebCommand::SaveMethodSettings,
      WebCommand::SaveCalibration, WebCommand::SaveWifi,
      WebCommand::OtaUpload};
  for (size_t i = 0; i < sizeof(otaBlockedCommands) /
                              sizeof(otaBlockedCommands[0]); ++i) {
    CHECK(admitWebCommand(otaBlockedCommands[i], otaLocked) ==
          AdmissionResult::OtaLocked);
    CHECK(admitWebCommand(otaBlockedCommands[i], otaInProgress) ==
          AdmissionResult::OtaLocked);
  }

  const WebCommand stateBlockedCommands[] = {
      WebCommand::EnterReady, WebCommand::CalibratePumps,
      WebCommand::ResetSignalFilter, WebCommand::ManualTitrant,
      WebCommand::ManualSample, WebCommand::ManualSweep,
      WebCommand::ManualStop};
  for (size_t i = 0; i < sizeof(stateBlockedCommands) /
                              sizeof(stateBlockedCommands[0]); ++i) {
    CHECK(admitWebCommand(stateBlockedCommands[i], active) ==
          AdmissionResult::InvalidState);
    CHECK(admitWebCommand(stateBlockedCommands[i], calibrating) ==
          AdmissionResult::InvalidState);
  }

  CHECK(admitWebCommand(WebCommand::EmergencyStop, unauthenticated) ==
        AdmissionResult::Allowed);
  CHECK(admitWebCommand(WebCommand::EmergencyStop, otaInProgress) ==
        AdmissionResult::Allowed);
  CHECK(admitWebCommand(WebCommand::Reset, otaLocked) == AdmissionResult::Allowed);
  CHECK(admitWebCommand(WebCommand::Reset, otaInProgress) == AdmissionResult::Allowed);
  CHECK(admitWebCommand(WebCommand::Logout, otaLocked) == AdmissionResult::Allowed);
  CHECK(admitWebCommand(WebCommand::Logout, otaInProgress) == AdmissionResult::Allowed);
  CHECK(admitWebCommand(WebCommand::Login, idle) == AdmissionResult::Allowed);
  CHECK(admitWebCommand(WebCommand::Recover, idle) == AdmissionResult::Allowed);
  CHECK(admitWebCommand(WebCommand::Login, otaLocked) == AdmissionResult::OtaLocked);
  CHECK(admitWebCommand(WebCommand::Recover, otaInProgress) == AdmissionResult::OtaLocked);
}

static void testLruSerialWrapPreservesRecency() {
  FakeCrypto crypto;
  AuthManager auth(crypto);
  auth.setAdministratorCredential(credential(crypto, "correct-password"));
  char first[33], second[33], third[33];
  CHECK(auth.login("correct-password", 16, 1, first) == AuthResult::Ok);
  CHECK(auth.login("correct-password", 16, 2, second) == AuthResult::Ok);
  AuthManagerTestAccess::forceSerial(auth, UINT32_MAX - 1U);
  uint8_t slot = 0;
  CHECK(auth.validateSession(first, 3, slot) == AuthResult::Ok);
  CHECK(auth.validateSession(second, 4, slot) == AuthResult::Ok);
  CHECK(auth.login("correct-password", 16, 5, third) == AuthResult::Ok);
  CHECK(auth.validateSession(first, 6, slot) == AuthResult::Required);
  CHECK(auth.validateSession(second, 6, slot) == AuthResult::Ok);
  CHECK(auth.validateSession(third, 6, slot) == AuthResult::Ok);
}

int main() {
  testPasswordLimitsAndVerification();
  testLoginLockout();
  testDirectAdministratorAuthentication();
  testSessionsAndExpiry();
  testMalformedTokens();
  testRecovery();
  testCommandAdmissionPolicy();
  testLruSerialWrapPreservesRecency();
  if (failures != 0) return 1;
  std::puts("All auth core tests passed");
  return 0;
}
