# Web Authentication and Command Security Design

Date: 2026-07-10

## Goal

Protect every Web mutation on the K10 potentiometric titrator with an authenticated, expiring browser session and a centralized command-admission policy. Preserve public read-only monitoring and unauthenticated emergency stop while preventing unauthorised pump control, settings changes, calibration, and firmware upload.

## Scope

This phase includes:

- Administrator-password login and logout.
- Two RAM-resident browser sessions with 30-minute write inactivity expiry.
- Per-device factory recovery passwords injected during manufacturing and printed on device labels.
- Administrator-password recovery without K10 A/B hardware buttons.
- POST-only mutation routes for actions, settings, authentication, recovery, and OTA.
- Central command admission based on authentication, Run state, and OTA safety lock.
- Login and recovery attempt rate limiting.
- Browser UI migration from GET links/forms to authenticated POST requests.
- Native authentication tests, static Web safety tests, firmware builds, and CI integration.

This phase does not add TLS, cloud identity, multi-user roles, remote account management, curve persistence, a full Run module extraction, task watchdog configuration, or hardware pump interlocks.

## Constraints

- Password recovery must not depend on the K10 A/B hardware buttons.
- Every device has a different fixed factory recovery password printed on its physical label.
- A factory recovery password may only reset the administrator password; it never directly authorizes pump, settings, calibration, or OTA commands.
- `/json`, the main page, manuals, and guide remain publicly readable on the local network.
- Emergency stop remains available without an authenticated session, but it may only stop pumps.
- The first-phase HTTP OTA safety lock remains authoritative after authentication succeeds and upload begins.
- Real passwords, password hashes, salts, and session tokens must not be committed to Git or returned by Web status interfaces.

## Chosen Approach

Use device-managed RAM session tokens rather than HTTP Basic authentication or stateless signed tokens.

HTTP Basic authentication does not provide precise inactivity expiry or clean logout behavior. Stateless signed tokens add unnecessary signing, revocation, and key-rotation complexity for a single-user embedded controller. Two RAM sessions provide explicit expiry, bounded memory, immediate logout, and automatic invalidation when the device restarts.

## Modules

### AuthManager

`auth_manager.h/.cpp` owns password verification, session lifecycle, recovery verification, and authentication rate limits.

Its interface provides operations equivalent to:

- Set or replace the administrator password.
- Verify an administrator password and create a session.
- Validate a session token for a write request.
- Record a successful write and refresh that session's inactivity timestamp.
- Log out one session.
- Clear all sessions.
- Verify the device factory recovery password.
- Report generic authentication failures without exposing which credential check failed.

AuthManager holds no pump or Run state knowledge.

### CommandAdmission

`command_admission.h/.cpp` decides whether an already parsed command may execute. Its inputs are command kind, authentication result, current Run state, HTTP OTA lock state, and HTTP OTA progress state.

Command categories include:

- Emergency stop.
- Run start/resume/pause/reset.
- Calibration and tare.
- Manual pump and PWM sweep.
- Method and device settings.
- WiFi settings and restart.
- OTA upload.
- Authentication and password recovery.

Emergency stop is the only mutation admitted without a session. OTA recovery reset remains governed by the first-phase OTA safety rules. CommandAdmission does not execute hardware operations; the Web adapter calls existing implementation only after admission succeeds.

### Web Adapter

The existing ESP32 WebServer routes parse requests, extract the `X-Session-Token` header, call AuthManager, call CommandAdmission, execute admitted operations, and encode JSON responses.

Static HTML and JavaScript remain in the current Web implementation during this phase. Full Web module extraction is deferred.

## Password Storage

Administrator passwords are 10 to 64 UTF-8 bytes. AuthManager generates a random 16-byte salt and stores only salt plus PBKDF2-HMAC-SHA256 output in Preferences. Password plaintext is discarded after verification or update.

Factory recovery passwords are 16 random characters chosen from a set that excludes visually ambiguous characters. The manufacturing script produces:

- A plaintext label value for the physical device label.
- A generated local build input containing only the recovery salt and derived hash.

The generated local build input is ignored by Git. The repository contains an example input and a generation script, but no valid device credential. Firmware must fail its production build configuration when a real factory recovery hash is missing; a clearly marked development-only credential may be enabled only through an explicit build flag.

Factory recovery plaintext is never stored in Preferences, returned through HTTP, or printed to the serial log.

## Cryptography and Randomness

- Password derivation: PBKDF2-HMAC-SHA256 using the ESP32 mbedTLS implementation.
- Administrator salt: 16 random bytes.
- Session token: 128 random bits encoded as 32 hexadecimal characters.
- Random source: ESP32 hardware-backed random generator.
- Credential comparison: constant-time comparison of derived hashes.

The PBKDF2 iteration count is fixed in the implementation after measuring acceptable K10 login latency and is stored alongside a credential format version so future firmware can migrate it deliberately.

## Session Lifecycle

The device stores at most two sessions in RAM. Each record contains token hash or token bytes, creation metadata required for validation, and last successful write time.

- Login creates a new random session.
- A third login evicts the least recently used session.
- Browser JavaScript stores the token in `sessionStorage`.
- Closing the browser tab removes the browser copy.
- Device restart removes all device session records.
- A session expires after 30 minutes without a successful authenticated write.
- `/json` polling and other reads never extend expiry.
- Every admitted write refreshes only the session that authorized it.
- Logout invalidates the presented session immediately.
- Administrator-password recovery clears all sessions.

Tokens never appear in URLs, redirect locations, HTML, `/json`, or logs.

## Routes and Methods

Public GET routes:

- `GET /` — Web UI.
- `GET /json` — read-only device status.
- Existing static/read-only guide content.

Authentication POST routes:

- `POST /login` — administrator password to session token.
- `POST /logout` — invalidate the presented token.
- `POST /recover` — factory recovery password plus new administrator password.

Authenticated mutation routes:

- `POST /action` — actions and manual commands.
- `POST /set` — settings and WiFi updates.
- `POST /ota` — firmware upload.

Legacy `GET /action` and `GET /set` return `405 Method Not Allowed` and never execute mutations. Login, logout, recover, and OTA also reject unsupported methods.

Every authenticated mutation carries `X-Session-Token`. The token header cannot be produced by a cross-origin HTML form, and no mutation accepts a URL token. The Web adapter also validates request origin/host where the ESP32 WebServer exposes sufficient request metadata, but custom-header authentication is the primary CSRF seam.

## Request Flow

### Login

1. Browser posts an administrator password to `/login`.
2. AuthManager checks the login rate limit.
3. AuthManager derives and compares the stored administrator credential.
4. Failure returns a generic `401` response and increments the login failure counter.
5. Success creates a random session, clears the login failure counter, and returns the token once.
6. Browser stores the token in `sessionStorage`.

### Authenticated Mutation

1. Browser posts to a mutation route with `X-Session-Token`.
2. AuthManager validates the token and inactivity deadline without refreshing it.
3. CommandAdmission evaluates command type, Run state, and OTA state.
4. Rejected commands return structured JSON and do not refresh the session.
5. The Web adapter executes an admitted command.
6. A successful command refreshes that session's last-write timestamp.

### Password Recovery

1. Browser posts the factory recovery password and a valid new administrator password to `/recover`.
2. The device immediately stops both pumps and prevents run commands for the duration of the recovery request.
3. AuthManager checks the recovery rate limit and verifies the factory recovery credential.
4. Failure returns a generic error, keeps pumps stopped, and does not modify the administrator credential.
5. Success stores a newly salted administrator credential, clears all sessions and authentication counters, clears current run data, and enters `SetupMode`.
6. The user must log in normally before executing any protected command.

## OTA Interaction

Authentication is evaluated before the HTTP upload implementation calls `Update.begin()`.

- Missing, invalid, or expired sessions reject OTA without entering the OTA safety lock.
- A valid session passes through CommandAdmission.
- Only then does the first-phase OTA flow stop pumps, assert the OTA lock, and begin flash writing.
- Upload success restarts into `SetupMode`, naturally clearing all RAM sessions.
- Upload failure keeps pumps locked and requires the existing Web safe reset behavior.
- The OTA lock prevents `/set`, ordinary actions, calibration, and hardware-button state changes exactly as implemented in phase one.

## Rate Limiting

Login:

- Five consecutive failures cause a 60-second lockout.
- Successful administrator login clears only the login failure counter.

Recovery:

- Three consecutive failures cause a five-minute lockout.
- Successful recovery clears all authentication failure counters and sessions.

Counters and temporary lockout deadlines live in RAM and clear on reboot. Credential hashes remain persistent. Responses disclose only that authentication is unavailable or failed, not the stored account state.

## Browser Behavior

The Web UI provides login, logout, and recovery forms. Existing action links and GET forms become JavaScript-driven POST requests.

- JavaScript reads the session token from `sessionStorage` and places it in `X-Session-Token`.
- A `401` response clears the browser token and opens the login view.
- A `403` response shows the command-state rejection without discarding a valid session.
- A `429` response shows the generic lockout message.
- A successful write refreshes the device-side session; the browser does not calculate authority locally.
- Without JavaScript, the UI remains read-only.

## Error Responses

Mutation routes return compact JSON with stable error codes:

- `AUTH_REQUIRED` — missing or invalid session.
- `AUTH_EXPIRED` — expired session.
- `AUTH_FAILED` — generic credential failure.
- `RATE_LIMITED` — login or recovery temporarily unavailable.
- `COMMAND_NOT_ALLOWED` — valid session but invalid current device state.
- `METHOD_NOT_ALLOWED` — unsupported HTTP method.
- Existing OTA failure codes remain mapped to the OTA safety state.

Responses never include passwords, salts, hashes, session-token values, or credential-existence details.

## Testing

Native tests cover:

- Administrator password hash success and failure.
- Password lengths below 10 bytes, above 64 bytes, and valid boundaries.
- Salt uniqueness and constant-time comparison behavior at the module interface.
- Session creation, validation, two-session capacity, LRU eviction, logout, and clearing.
- Thirty-minute inactivity expiry including `millis()` rollover.
- Read operations not refreshing sessions and successful writes refreshing them.
- Login failure count and 60-second lockout.
- Recovery failure count and five-minute lockout.
- Successful recovery clearing all sessions and counters.
- CommandAdmission rules for every command category, Run state, and OTA state.
- Emergency stop admission without a session.

Static Web safety tests cover:

- GET mutation routes cannot execute handlers.
- Every protected POST handler invokes authentication and CommandAdmission before mutation.
- `/ota` authenticates before `Update.begin()`.
- Tokens never appear in URL construction, `/json`, or logs.
- Sensitive credential fields are absent from `/json`.
- Recovery stops both pumps and enters `SetupMode` only after successful verification.

Integration verification includes:

- PlatformIO firmware build.
- Native host test suite.
- Existing OTA/endpoint static safety suite.
- Browser login, expiry, logout, recovery, action, settings, panic, and OTA smoke tests against a K10 device.
- GitHub Actions running the host tests and firmware build.

## Migration and Provisioning

Existing devices without an administrator credential start in a locked provisioning state for mutations. Read-only monitoring and unauthenticated emergency stop remain available. The operator uses the device-specific factory recovery password from the new physical label to set the first administrator password, after which normal login is required.

Installing this firmware does not erase existing methods, pump calibration, pH calibration, or WiFi settings. Authentication data uses a dedicated Preferences namespace and versioned keys.

## Future Phases

After this phase:

1. Extract a host-testable Run module that owns transitions, timers, safety invariants, and pump intents.
2. Validate pump calibration values and bind calibration validity to pump speed.
3. Improve slope estimation using recorded-curve replay tests.
4. Add resilient experiment records without high-frequency Flash writes.

