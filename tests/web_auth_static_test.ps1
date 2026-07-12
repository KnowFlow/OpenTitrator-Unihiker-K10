$ErrorActionPreference = 'Stop'
$sketch = Get-Content -Raw (Join-Path $PSScriptRoot '..\ph_titrator\ph_titrator.ino')
function Need([string]$p,[string]$m) { if ($sketch -notmatch $p) { throw "FAIL: $m" } }
Need 'server\.on\("/action",\s*HTTP_POST,\s*handleAction\)' '/action must be POST'
Need 'server\.on\("/set",\s*HTTP_POST,\s*handleSet\)' '/set must be POST'
Need 'server\.on\("/action",\s*HTTP_GET,\s*handleMethodNotAllowed\)' 'GET /action must be 405'
Need 'server\.on\("/set",\s*HTTP_GET,\s*handleMethodNotAllowed\)' 'GET /set must be 405'
foreach ($route in 'login','logout','recover') { Need ('server\.on\("/' + $route + '",\s*HTTP_POST') "/$route must be POST-only" }
Need 'void handleSet\(\) \{\s*uint8_t sessionSlot;\s*if \(!requireCommand\(WebCommand::SaveMethodSettings, sessionSlot\)\)' '/set must authenticate/admit before mutation'
Need 'void handleAction\(\) \{[\s\S]*?String cmd = server\.arg\("cmd"\);[\s\S]*?requireCommand\(' '/action must parse then authenticate/admit'
Need 'UPLOAD_FILE_START[\s\S]*?validateSession[\s\S]*?admitWebCommand\(WebCommand::OtaUpload[\s\S]*?enterHttpOtaSafety\(\)[\s\S]*?Update\.begin' 'OTA authentication/admission must precede safety and Update.begin'
Need 'void handleRecover\(\) \{\s*pump\.stop\(\);\s*samplePump\.stop\(\);[\s\S]*?authManager\.recover[\s\S]*?resetRunData\(\);[\s\S]*?RunState::SetupMode' 'recovery ordering is unsafe'
Need 'void handlePanic\(\) \{\s*pump\.stop\(\);\s*samplePump\.stop\(\);' 'anonymous panic must only stop pumps'
Need 'bool otaUploadStartSeen = false;' 'OTA lifecycle must track a current start event'
Need 'void clearOtaRequestState\(\)' 'OTA lifecycle needs one explicit clearing operation'
Need 'void handleOta\(\) \{\s*if \(!otaUploadStartSeen \|\| !otaRequestAccepted\)' 'empty/malformed OTA requests must be rejected'
Need 'void handleOta\(\) \{[\s\S]*?clearOtaRequestState\(\);\s*\}' 'OTA final handler must clear request state'
Need 'UPLOAD_FILE_START\) \{\s*clearOtaRequestState\(\);\s*otaUploadStartSeen = true;' 'each OTA start must reset stale authorization'
Need 'UPLOAD_FILE_ABORTED\) \{[\s\S]*?clearOtaRequestState\(\);' 'OTA abort must clear request state'
Need 'struct SettingsCandidate[\s\S]*?SettingsCandidate candidate =' '/set must stage request values'
Need 'if \(!validateSettingsCandidate\(candidate' '/set must validate before commit'
Need 'commitSettingsCandidate\(candidate\);[\s\S]*?recordSuccessfulWrite' '/set must commit before refreshing session'
Need 'resetFromHttpOtaFailure\(\);\s*authManager\.recordSuccessfulWrite\(sessionSlot, millis\(\)\);' 'successful terminal OTA reset must refresh session'
$setBeforeValidation = [regex]::Match($sketch, 'void handleSet\(\) \{([\s\S]*?)if \(!validateSettingsCandidate\(candidate').Groups[1].Value
if (-not $setBeforeValidation) { throw 'FAIL: could not inspect /set staging prefix' }
if ($setBeforeValidation -cmatch 'save[A-Z]|setRunPulseUs|setCalibrationFactor|selectMethod|setState|resetRunData') {
  throw 'FAIL: /set performs a side effect before whole-request validation'
}
$rejectedOtaWrite = [regex]::Match($sketch, 'else if \(upload\.status == UPLOAD_FILE_WRITE\) \{([\s\S]*?)\n  \}').Groups[1].Value
if ($rejectedOtaWrite -notmatch '^\s*if \(!otaRequestAccepted\) return;') { throw 'FAIL: rejected OTA chunks can reach Update.write' }
$json = [regex]::Match($sketch, 'void handleJson\(\) \{([\s\S]*?)\n\}').Groups[1].Value
if ($json -match 'password|salt|hash|session.?token') { throw 'FAIL: /json exposes authentication material' }
Write-Host 'All web authentication static tests passed'
