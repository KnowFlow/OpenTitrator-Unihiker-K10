# USB Authentication Recovery Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Provision a new administrator credential through a USB-only K10 recovery image without changing other device settings.

**Architecture:** A PlatformIO build flag excludes the normal sketch and enables a minimal recovery entrypoint. It consumes an ignored, host-generated administrator hash header and reuses `AuthStore::saveAdministrator` for atomic persistence.

**Tech Stack:** Arduino/ESP32, Preferences, PlatformIO, Python unittest, PowerShell, ESP32 USB serial.

## Global Constraints

- Recovery is available only in the `unihiker-auth-recovery` USB build environment.
- Password plaintext, salts, hashes, labels, and session tokens remain ignored and unlogged.
- Only the `auth` Preferences namespace may be changed.
- Pump commands, calibration settings, and Wi-Fi settings are out of scope.

---

### Task 1: Add a testable recovery build boundary

**Files:**
- Modify: `platformio.ini`
- Modify: `tests/auth_store_protocol_test.py`

**Interfaces:**
- Produces the `unihiker-auth-recovery` environment with `AUTH_USB_RECOVERY=1`.

- [ ] **Step 1: Write the failing test**

```python
def test_recovery_environment_is_explicit_and_isolated(self):
    platformio = (ROOT / "platformio.ini").read_text()
    self.assertIn("[env:unihiker-auth-recovery]", platformio)
    self.assertIn("-DAUTH_USB_RECOVERY=1", platformio)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python -m unittest tests.auth_store_protocol_test.AuthStoreProtocolTest.test_recovery_environment_is_explicit_and_isolated`

Expected: failure because the environment does not yet exist.

- [ ] **Step 3: Add the recovery environment**

```ini
[env:unihiker-auth-recovery]
extends = env:unihiker
build_flags =
    ${env:unihiker.build_flags}
    -DAUTH_USB_RECOVERY=1
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `python -m unittest tests.auth_store_protocol_test.AuthStoreProtocolTest.test_recovery_environment_is_explicit_and_isolated`

Expected: `OK`.

### Task 2: Add the USB-only recovery entrypoint

**Files:**
- Modify: `ph_titrator/auth_store.h`
- Modify: `ph_titrator/auth_store.cpp`
- Modify: `ph_titrator/ph_titrator.ino`
- Create: `ph_titrator/auth_usb_recovery.cpp`
- Modify: `tests/auth_store_protocol_test.py`

**Interfaces:**
- Consumes: `AuthStore::saveAdministrator(const AuthCredential&)`.
- Produces: `AuthStore::loadRecoveryAdministrator(AuthCredential&)` and recovery serial marker `AUTH_RECOVERY:OK`.

- [ ] **Step 1: Write the failing test**

```python
def test_recovery_entrypoint_only_uses_admin_store(self):
    recovery = (ROOT / "ph_titrator" / "auth_usb_recovery.cpp").read_text()
    self.assertIn("authStore.saveAdministrator", recovery)
    self.assertIn('Serial.println("AUTH_RECOVERY:OK")', recovery)
    self.assertNotIn("prefs.clear", recovery)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python -m unittest tests.auth_store_protocol_test.AuthStoreProtocolTest.test_recovery_entrypoint_only_uses_admin_store`

Expected: failure because the entrypoint does not yet exist.

- [ ] **Step 3: Implement the minimum recovery image**

```cpp
#ifdef AUTH_USB_RECOVERY
void setup() {
  Serial.begin(115200);
  AuthCredential credential;
  if (!authStore.loadRecoveryAdministrator(credential) ||
      !authStore.saveAdministrator(credential)) {
    Serial.println("AUTH_RECOVERY:FAIL");
    return;
  }
  Serial.println("AUTH_RECOVERY:OK");
}
void loop() {}
#endif
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `python -m unittest tests.auth_store_protocol_test.AuthStoreProtocolTest.test_recovery_entrypoint_only_uses_admin_store`

Expected: `OK`.

### Task 3: Generate ignored local recovery input and verify builds

**Files:**
- Create: `scripts/generate_recovery_admin.py`
- Modify: `.gitignore`

**Interfaces:**
- Produces ignored `ph_titrator/recovery_admin.generated.h` with only PBKDF2 credential material.

- [ ] **Step 1: Write the failing generator test**

```python
def test_recovery_header_contains_no_plaintext(self):
    header = generate_header("example-password")
    self.assertNotIn("example-password", header)
    self.assertIn("RECOVERY_ADMIN_HASH", header)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python -m unittest tests.factory_auth_generator_test`

Expected: failure until the recovery generator is imported and tested.

- [ ] **Step 3: Implement generator and ignore its output**

Use `hashlib.pbkdf2_hmac("sha256", ...)`, a 16-byte random salt, 120000 iterations, and atomic local output replacement. Reject passwords outside 10–64 UTF-8 bytes.

- [ ] **Step 4: Build both environments**

Run: `pio run -e unihiker` and `pio run -e unihiker-auth-recovery` with the required ignored headers temporarily present.

Expected: both exit successfully.

### Task 4: USB recovery procedure

**Files:**
- Modify: `docs/MANUAL_CN.md`

- [ ] **Step 1: Document the precise recovery sequence**

Document: generate the ignored recovery header locally; flash `unihiker-auth-recovery` over USB; watch for `AUTH_RECOVERY:OK`; flash `unihiker`; login with the newly chosen administrator password; delete the ignored header.

- [ ] **Step 2: Device verification**

Run: flash recovery to the selected USB port, observe its marker, flash normal firmware, POST `/login` with the new password, and verify only a session token is returned.

