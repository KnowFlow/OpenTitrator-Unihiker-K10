# Final Review Fix Report

Date: 2026-07-12

## Changes

- PowerShell OTA upload now constructs a real `multipart/form-data` request with a quoted `name="file"`, quoted firmware filename, binary stream content, and `X-Session-Token`. Errors remain generic and never print the token.
- The fixed-capacity authentication LRU renormalizes active-session serial ranks before `uint32_t` overflow, preserving eviction order without heap allocation.
- `/set` validates pH calibration spans only when calibration-related fields are supplied.
- Anonymous `/panic` explicitly admits `WebCommand::EmergencyStop` through the centralized policy before its pump-stop-only action.

## Verification

- `tests/control_test.exe` — passed: `All ph titrator control tests passed`.
- `tests/auth_core_test.exe` — passed: `All auth core tests passed`, including forced LRU serial-wrap eviction.
- `pwsh -NoProfile -File tests/ota_upload_script_test.ps1` — passed. Injectable transport inspected the serialized multipart boundary, quoted file disposition/filename, exact binary firmware bytes, token header, and token-free failure output.
- Parent-process exit verification: `pwsh -NoProfile -Command '& pwsh -NoProfile -File tests/ota_upload_script_test.ps1; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }; exit 0'` returned exit code 0 after all assertions and the success message.
- `pwsh -NoProfile -File tests/web_auth_static_test.ps1` — passed.
- `pwsh -NoProfile -File tests/sketch_safety_static_test.ps1` — passed.
- Python unit suites could not be freshly executed because the managed sandbox denied access to each installed Python interpreter.
- Provisioned PlatformIO/compile-server build could not be run: no local `pio` executable and no configured `COMPILE_SERVER` were available in this environment.

No credentials, credential-derived generated headers, label files, or session tokens are included in this report.
