# Web Authentication and Command Security Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Require an expiring administrator session for every Web mutation except emergency stop, and provide per-device factory-password recovery without hardware buttons.

**Architecture:** Build a fixed-capacity, host-testable authentication module around injected crypto and persistence adapters; build a pure command-admission module; then migrate the ESP32 Web adapter from GET mutations to authenticated POST requests. Keep OTA flash safety, pump execution, and the existing Run implementation in the sketch.

**Tech Stack:** Arduino C++ on UNIHIKER K10 / ESP32-S3, mbedTLS PBKDF2-HMAC-SHA256, ESP32 hardware RNG, Preferences, ESP32 WebServer, PlatformIO, native MSVC C++ tests, PowerShell static safety tests, Python 3 provisioning script.

## Global Constraints

- Password recovery must not depend on K10 A/B hardware buttons.
- Every device has a different fixed factory recovery password printed on its physical label.
- Factory recovery may only replace the administrator password; it never directly authorizes pumps, settings, calibration, or OTA.
- `/json`, the main page, manuals, and guide remain publicly readable.
- Emergency stop remains unauthenticated and may only stop pumps.
- `/action`, `/set`, `/ota`, `/login`, `/logout`, and `/recover` are POST-only.
- Session tokens use `X-Session-Token`; tokens never appear in URLs, `/json`, HTML, redirects, or logs.
- Sessions are RAM-only, limited to two, expire after 30 minutes without a successful write, and disappear on restart.
- Login locks for 60 seconds after five consecutive failures; recovery locks for five minutes after three consecutive failures.
- Administrator passwords are 10–64 UTF-8 bytes; factory passwords are 16 non-ambiguous random characters.
- Existing OTA safety lock, endpoint Hold, pump PWM/burst behavior, dosing decisions, methods, and calibration behavior remain unchanged.
- No real factory password, salt, hash, administrator password, or session token may be committed.

---

## File Structure

- Create `ph_titrator/auth_core.h` and `ph_titrator/auth_core.cpp`: fixed-capacity sessions, rate limits, password-policy validation, and generic credential operations.
- Create `ph_titrator/auth_crypto_esp32.h` and `ph_titrator/auth_crypto_esp32.cpp`: mbedTLS PBKDF2, constant-time comparison, and ESP32 random bytes.
- Create `ph_titrator/auth_store.h` and `ph_titrator/auth_store.cpp`: versioned authentication Preferences records and factory credential access.
- Create `ph_titrator/command_admission.h`: pure command/state policy.
- Create `ph_titrator/factory_auth.example.h`: non-secret structure example only.
- Generate but ignore `ph_titrator/factory_auth.generated.h` and `factory-label.txt`.
- Create `scripts/generate_factory_auth.py`: per-device recovery credential generator.
- Create `tests/auth_core_test.cpp`: native authentication and command policy tests.
- Create `tests/web_auth_static_test.ps1`: route/method/order/sensitive-field invariants.
- Modify `ph_titrator/ph_titrator.ino`: adapters, routes, POST parsing, recovery, command admission, and browser UI.
- Modify `.gitignore`, `platformio.ini`, `.github/workflows/ci.yml`, README/manual files, and existing static test runner inputs.

### Task 1: Build the Host-Testable Authentication Core

**Files:**
- Create: `ph_titrator/auth_core.h`
- Create: `ph_titrator/auth_core.cpp`
- Create: `tests/auth_core_test.cpp`

**Interfaces:**
- Produces: `AuthCredential`, `AuthCrypto`, `AuthSession`, `AuthResult`, and `AuthManager`.
- Produces: `AuthManager::login`, `validateSession`, `recordSuccessfulWrite`, `logout`, `recover`, `clearSessions`, and `setAdministratorCredential`.
- Consumes: caller-provided `uint32_t now`; caller-provided crypto and credential-save adapters.

- [ ] **Step 1: Define the public authentication types in a failing test**

Create `tests/auth_core_test.cpp` with a deterministic `FakeCrypto` and in-memory save callback. The test must compile against this intended interface:

```cpp
#include "../ph_titrator/auth_core.h"

struct FakeCrypto : AuthCrypto {
  uint8_t nextRandom = 1;
  bool derive(
      const char *password,
      size_t passwordBytes,
      const uint8_t *salt,
      size_t saltBytes,
      uint32_t iterations,
      uint8_t out[AuthCredential::HashBytes]) override;
  void randomBytes(uint8_t *out, size_t count) override;
};

static bool saveAdmin(void *context, const AuthCredential &credential) {
  *static_cast<AuthCredential *>(context) = credential;
  return true;
}
```

Test password byte limits 9/10/64/65, correct and incorrect credential verification, five-login failure lockout for 60 seconds, three-recovery failure lockout for 300 seconds, two-session capacity, LRU eviction, logout, clear-all, 30-minute expiry across `UINT32_MAX`, reads not refreshing expiry, successful writes refreshing expiry, and successful recovery saving a new credential and clearing sessions/counters.

- [ ] **Step 2: Run native compilation and confirm RED**

Run on Windows:

```powershell
New-Item -ItemType Directory -Force build | Out-Null
$cmd = 'call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul & cl /nologo /std:c++17 /EHsc /W4 tests\auth_core_test.cpp /Fe:build\auth_core_test.exe'
cmd.exe /d /c $cmd
```

Expected: compilation fails because `auth_core.h` does not exist.

- [ ] **Step 3: Implement the fixed-capacity interface**

Create `ph_titrator/auth_core.h` with these constants and shapes:

```cpp
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
  virtual bool derive(const char *, size_t, const uint8_t *, size_t,
                      uint32_t, uint8_t out[AuthCredential::HashBytes]) = 0;
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
  AuthResult login(const char *password, size_t bytes, uint32_t now,
                   char tokenHex[33]);
  AuthResult validateSession(const char *tokenHex, uint32_t now, uint8_t &slot);
  void recordSuccessfulWrite(uint8_t slot, uint32_t now);
  AuthResult logout(const char *tokenHex);
  void clearSessions();
  AuthResult recover(const char *factoryPassword, size_t factoryBytes,
                     const char *newPassword, size_t newBytes, uint32_t now,
                     SaveCredentialFn save, void *saveContext);
};
```

Implement with fixed arrays only: no `String`, heap allocation, STL containers, or token storage outside the two `AuthSession` records. Use unsigned elapsed subtraction for rollover. Decode tokens only when exactly 32 hexadecimal characters; compare token bytes without early exit. Increment an LRU serial on validation and successful writes; evict the active session with the smallest serial.

- [ ] **Step 4: Run RED/GREEN native tests**

Compile and run:

```powershell
$cmd = 'call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul & cl /nologo /std:c++17 /EHsc /W4 tests\auth_core_test.cpp ph_titrator\auth_core.cpp /Fe:build\auth_core_test.exe & if errorlevel 1 exit /b 1 & build\auth_core_test.exe'
cmd.exe /d /c $cmd
```

Expected: `All auth core tests passed` and exit 0.

- [ ] **Step 5: Commit Task 1**

```powershell
git add ph_titrator/auth_core.h ph_titrator/auth_core.cpp tests/auth_core_test.cpp
git commit -m "Add embedded authentication core"
```

### Task 2: Add Command Admission Policy

**Files:**
- Create: `ph_titrator/command_admission.h`
- Modify: `tests/auth_core_test.cpp`

**Interfaces:**
- Produces: `WebCommand`, `AdmissionContext`, and `admitWebCommand`.
- Consumes: existing `RunState` semantics through a small independent `AdmissionRunState` enum to keep host tests Arduino-free.

- [ ] **Step 1: Add failing policy-table tests**

Extend `tests/auth_core_test.cpp` to cover each command with authenticated/unauthenticated, active/idle, OTA locked/in-progress contexts. Use this interface:

```cpp
enum class WebCommand : uint8_t {
  EmergencyStop, Start, StartExisting, Pause, Reset, Tare,
  EnterReady, CalibratePumps, ResetSignalFilter,
  ManualTitrant, ManualSample, ManualSweep, ManualStop,
  SaveMethodSettings, SaveCalibration, SaveWifi,
  OtaUpload, Login, Logout, Recover
};

struct AdmissionContext {
  bool authenticated;
  bool otaSafetyLock;
  bool otaInProgress;
  bool runActive;
  bool calibrating;
};

enum class AdmissionResult : uint8_t {
  Allowed, AuthenticationRequired, OtaLocked, InvalidState
};
```

Required assertions: emergency stop always allowed; login/recover allowed only when OTA is not active; every other mutation requires authentication; OTA upload requires authentication and no OTA lock; manual/calibration commands reject active runs; settings reject OTA lock; reset follows existing terminal-OTA recovery rules in the sketch after admission.

- [ ] **Step 2: Confirm RED, implement, and confirm GREEN**

Create header-only `ph_titrator/command_admission.h` with one exhaustive `switch`. Compile with `/W4`; no default branch may silently allow a new enum value. Run `build/auth_core_test.exe` and expect all tests to pass.

- [ ] **Step 3: Commit Task 2**

```powershell
git add ph_titrator/command_admission.h tests/auth_core_test.cpp
git commit -m "Add web command admission policy"
```

### Task 3: Implement ESP32 Crypto, Persistence, and Factory Provisioning

**Files:**
- Create: `ph_titrator/auth_crypto_esp32.h`
- Create: `ph_titrator/auth_crypto_esp32.cpp`
- Create: `ph_titrator/auth_store.h`
- Create: `ph_titrator/auth_store.cpp`
- Create: `ph_titrator/factory_auth.example.h`
- Create: `scripts/generate_factory_auth.py`
- Create: `tests/factory_auth_generator_test.py`
- Modify: `.gitignore`
- Modify: `platformio.ini`

**Interfaces:**
- Produces: `Esp32AuthCrypto : AuthCrypto`.
- Produces: `AuthStore::loadAdministrator`, `saveAdministrator`, and `loadFactory`.
- Produces: generator outputs `ph_titrator/factory_auth.generated.h` and a user-selected label file.

- [ ] **Step 1: Write failing generator tests**

Create `tests/factory_auth_generator_test.py` using `unittest` and `tempfile`. Invoke the generator twice and assert:

- Each plaintext factory password is 16 characters.
- Password characters exclude `0O1Il`.
- Two runs produce different passwords and salts.
- Generated headers contain version, iteration count, 16 salt bytes, and 32 hash bytes.
- Generated headers never contain the plaintext password.
- Label output contains the plaintext and a device identifier.

Run `python -m unittest tests.factory_auth_generator_test`; expect import/file failure.

- [ ] **Step 2: Implement the provisioning script**

Create `scripts/generate_factory_auth.py` with CLI:

```text
python scripts/generate_factory_auth.py --device-id K10-001 \
  --header ph_titrator/factory_auth.generated.h \
  --label factory-label.txt --iterations 120000
```

Use `secrets.choice`, `secrets.token_bytes(16)`, and `hashlib.pbkdf2_hmac('sha256', ...)`. Refuse to overwrite either output without `--force`. Write the header with only byte arrays and metadata; write plaintext only to the label file.

- [ ] **Step 3: Ignore secret outputs and provide a structure example**

Add to `.gitignore`:

```gitignore
ph_titrator/factory_auth.generated.h
factory-label*.txt
```

Create `factory_auth.example.h` containing zero/invalid values and comments, never an accepted recovery credential.

- [ ] **Step 4: Implement ESP32 adapters**

`Esp32AuthCrypto::derive` uses mbedTLS PBKDF2-HMAC-SHA256 and returns false on any mbedTLS error. `randomBytes` fills through `esp_fill_random`. `AuthStore` uses a dedicated `auth` Preferences namespace with versioned keys `admin_ver`, `admin_iter`, `admin_salt`, and `admin_hash` and validates exact byte lengths before accepting records.

`loadFactory` includes `factory_auth.generated.h` only when present. Add a build-time guard: ordinary device builds fail with a clear message when the generated header is absent. CI invokes the generator with a throwaway credential immediately before `pio run`, then deletes generated outputs.

- [ ] **Step 5: Add sources to PlatformIO and verify**

Ensure the sketch build compiles all new `.cpp` files in `src_dir = ph_titrator`. Run generator tests and `pio run --environment unihiker`; expect both pass.

- [ ] **Step 6: Commit Task 3**

```powershell
git add .gitignore platformio.ini ph_titrator/auth_crypto_esp32.* ph_titrator/auth_store.* ph_titrator/factory_auth.example.h scripts/generate_factory_auth.py tests/factory_auth_generator_test.py
git commit -m "Add device authentication provisioning"
```

### Task 4: Integrate Authentication and POST-Only Firmware Routes

**Files:**
- Modify: `ph_titrator/ph_titrator.ino`
- Create: `tests/web_auth_static_test.ps1`
- Modify: `tests/sketch_safety_static_test.ps1`

**Interfaces:**
- Consumes: `AuthManager`, `Esp32AuthCrypto`, `AuthStore`, and `admitWebCommand`.
- Produces: POST `/login`, `/logout`, `/recover`, `/action`, `/set`, and authenticated POST `/ota`.

- [ ] **Step 1: Write failing static route/order tests**

Create `tests/web_auth_static_test.ps1` that reads the sketch as one string and fails unless all of these hold:

- `/action` and `/set` register explicit `HTTP_POST` handlers.
- GET `/action` and `/set` register handlers that return 405 and never call mutation handlers.
- `/login`, `/logout`, and `/recover` are POST-only.
- Protected handlers call session validation and command admission before existing mutation operations.
- OTA request authentication occurs before `enterHttpOtaSafety()` and `Update.begin()`.
- Recovery stops both pumps before verification and calls `resetRunData`/`SetupMode` only on success.
- `/json` construction contains no password, salt, hash, or session token fields.
- `panic` has an explicit unauthenticated POST admission path limited to pump stop.

Run it and expect failure before integration.

- [ ] **Step 2: Initialize authentication during setup**

Add global adapters and manager. During `setup()`, load factory and administrator credentials before starting protected Web routes. Authentication storage failure leaves mutations locked but preserves `/json` and emergency stop.

- [ ] **Step 3: Add JSON response and authentication helpers**

Add helpers equivalent to:

```cpp
void sendApiError(int status, const char *code, const String &message);
bool readSessionToken(char out[33]);
bool requireSession(uint8_t &slot, bool refreshAfterSuccess = false);
AdmissionRunState admissionRunState();
bool requireCommand(WebCommand command, uint8_t &sessionSlot);
```

Register `X-Session-Token` with `server.collectHeaders` before `server.begin()`.

- [ ] **Step 4: Implement login/logout/recovery**

`/login` accepts password form data, returns `{ok:true,token:"..."}` once, and maps failures to `401`/`429`. `/logout` requires and invalidates the current session. `/recover` stops both pumps immediately, validates `factory_password` and `new_password`, persists the new administrator record through AuthStore, clears sessions/run data, and enters `SetupMode` only on success.

- [ ] **Step 5: Convert action/settings routes**

Register mutation implementations only as POST. Parse the command first, map it to `WebCommand`, admit it, execute existing logic, then call `recordSuccessfulWrite` only after success. Reject unknown commands without refreshing the session. Preserve the first-phase OTA lock and safe-reset rules.

- [ ] **Step 6: Authenticate OTA before upload starts**

Because ESP32 WebServer invokes upload callbacks before the final handler, validate `X-Session-Token` and OTA admission at `UPLOAD_FILE_START` before `enterHttpOtaSafety()`. Store only a boolean/slot for the duration of that request; never log the token. If authentication fails, do not call `Update.begin`, consume/reject remaining chunks, and return `401` or `403` from the final handler.

- [ ] **Step 7: Run static, native, and firmware checks**

Run:

```powershell
./tests/web_auth_static_test.ps1
./tests/sketch_safety_static_test.ps1
build/auth_core_test.exe
pio run --environment unihiker
```

Expected: all pass.

- [ ] **Step 8: Commit Task 4**

```powershell
git add ph_titrator/ph_titrator.ino tests/web_auth_static_test.ps1 tests/sketch_safety_static_test.ps1
git commit -m "Protect web mutation routes"
```

### Task 5: Migrate the Browser UI to Session POST Requests

**Files:**
- Modify: `ph_titrator/ph_titrator.ino`
- Modify: `tests/web_auth_static_test.ps1`

**Interfaces:**
- Consumes: `/login`, `/logout`, `/recover`, POST `/action`, POST `/set`, and `X-Session-Token`.
- Produces: login/recovery UI and one JavaScript `apiPost` mutation seam.

- [ ] **Step 1: Extend static tests for browser invariants**

Require that generated HTML/JS contains no `href='/action?`, no `method='get'` for mutation forms, no `fetch('/action?`, and no URL/session-token concatenation. Require `sessionStorage`, `X-Session-Token`, login/logout/recovery forms, and a single POST helper.

- [ ] **Step 2: Add authentication UI**

Add compact login, logout, and factory recovery panels. Recovery requires factory password, new password, and confirmation; browser validation enforces matching values and 10–64 UTF-8 bytes before POST. Do not prefill or display any recovery credential.

- [ ] **Step 3: Centralize browser mutations**

Implement:

```javascript
function sessionToken(){return sessionStorage.getItem('k10_session')||''}
async function apiPost(path,form,allowAnonymous){
  const headers={'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'};
  const token=sessionToken();
  if(token)headers['X-Session-Token']=token;
  const response=await fetch(path,{method:'POST',headers,body:new URLSearchParams(form)});
  const data=await response.json();
  if(response.status===401){sessionStorage.removeItem('k10_session');showLogin()}
  if(!response.ok)throw new Error(data.message||data.code||'Request failed');
  return data;
}
```

Every action button and settings/manual form uses this helper. Emergency stop calls it with anonymous permission but still includes a token when available. OTA upload uses `fetch` POST with `FormData` and the same token header.

- [ ] **Step 4: Verify UI source and firmware**

Run both static suites and PlatformIO build. Open the device page in a browser only after firmware deployment; browser smoke testing is Task 7.

- [ ] **Step 5: Commit Task 5**

```powershell
git add ph_titrator/ph_titrator.ino tests/web_auth_static_test.ps1
git commit -m "Add authenticated web session UI"
```

### Task 6: Update CI, Documentation, and Provisioned Build Workflow

**Files:**
- Modify: `.github/workflows/ci.yml`
- Modify: `README.md`
- Modify: `README_CN.md`
- Modify: `docs/MANUAL.md`
- Modify: `docs/MANUAL_CN.md`
- Modify: `scripts/ota_upload.py`
- Modify: `scripts/ota_upload.ps1`

**Interfaces:**
- Consumes: factory generator, native auth tests, POST routes, and session-token OTA.
- Produces: reproducible CI and operator/manufacturing instructions.

- [ ] **Step 1: Extend CI**

Before firmware build, generate a throwaway `CI-ONLY` factory credential, build, and delete the generated header/label. Add MSVC-independent Linux commands:

```bash
g++ -std=c++17 -Wall -Wextra -pedantic tests/auth_core_test.cpp ph_titrator/auth_core.cpp -o auth_core_test
./auth_core_test
python -m unittest tests.factory_auth_generator_test
pwsh ./tests/web_auth_static_test.ps1
pwsh ./tests/sketch_safety_static_test.ps1
```

- [ ] **Step 2: Update OTA upload scripts**

Require `--token`/`-Token`, send `X-Session-Token`, and refuse upload when missing. Never print the token. Preserve firmware path/IP progress and response handling.

- [ ] **Step 3: Document operator and manufacturing workflows**

Document login, 30-minute inactivity, logout, Web-only recovery, label handling, first administrator setup, POST-only incompatibility for legacy integrations, authenticated OTA, and recovery entering `SetupMode`. State that HTTP remains local-network plaintext and does not protect against a network packet sniffer; use the device AP or trusted LAN.

- [ ] **Step 4: Run full automated verification**

Generate a temporary local credential, then run native control/auth suites, Python generator tests, both PowerShell static suites, and `pio run`. Run `git diff --check` and confirm no generated credential/header/label is tracked or printed in diffs.

- [ ] **Step 5: Commit Task 6**

```powershell
git add .github/workflows/ci.yml README.md README_CN.md docs/MANUAL.md docs/MANUAL_CN.md scripts/ota_upload.py scripts/ota_upload.ps1
git commit -m "Document authenticated device workflow"
```

### Task 7: Provision and Smoke-Test the Current Device

**Files:**
- Generated locally and ignored: `ph_titrator/factory_auth.generated.h`
- Generated locally and ignored: `factory-label-K10-current.txt`

**Interfaces:**
- Consumes: generator, provisioned PlatformIO build, login/recovery/action/settings/OTA routes.
- Produces: one labelled recovery credential for the physical device and verified deployed firmware.

- [ ] **Step 1: Generate the current device credential**

Run the generator with a user-approved device identifier. Display the label file only to the user, never in logs intended for Git. Ask the user to confirm the physical label has been attached before relying on recovery.

- [ ] **Step 2: Build and OTA the provisioned firmware**

Build after generation. Because the existing device runs pre-auth firmware while the repository upload scripts now require a token, perform exactly one migration upload with the raw HTTP endpoint:

```powershell
curl.exe --fail --show-error --form "file=@.pio/build/unihiker/firmware.bin" http://DEVICE_IP/ota
```

Use this only after confirming `/json` identifies the intended pre-auth device and both pumps are stopped. After restart, legacy unauthenticated OTA is gone; do not preserve a bypass flag or migration route in the new firmware or upload scripts.

- [ ] **Step 3: Perform browser/device smoke tests**

Against the K10 device verify:

- Public `/json` works without login.
- Protected action rejects no token.
- Emergency stop works without token and pumps remain stopped.
- Factory recovery sets the first administrator password and returns `SetupMode`.
- Login creates a token; start/tare/settings commands follow state policy.
- Logout invalidates the token.
- Wrong login/recovery attempts trigger configured limits without exposing details.
- Authenticated OTA succeeds, stops pumps, restarts, clears sessions, and returns `SetupMode`.

- [ ] **Step 4: Record verification without credentials**

Write a credential-free deployment note containing device ID, firmware commit, time, route results, and label-confirmation status. Do not record recovery/admin passwords or tokens.

## Self-Review

- Spec coverage: Tasks 1–2 cover host-testable auth and command policy; Task 3 covers crypto/storage/manufacturing; Tasks 4–5 cover firmware and browser integration; Task 6 covers CI/docs/tooling; Task 7 covers per-device provisioning and smoke deployment.
- Placeholder scan: no implementation placeholders or unspecified error-handling steps remain.
- Type consistency: `AuthCredential`, `AuthCrypto`, `AuthManager`, `WebCommand`, `AdmissionContext`, and route/header names remain consistent across tasks.
- Safety: authentication is always evaluated before OTA flash begin; recovery stops pumps and never grants direct control; emergency stop remains anonymous and stop-only.
- Scope: TLS, multi-user roles, Run extraction, curve storage, watchdogs, and hardware interlocks remain deferred.
