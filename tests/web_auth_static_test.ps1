$ErrorActionPreference = 'Stop'
$sketch = Get-Content -Raw (Join-Path $PSScriptRoot '..\ph_titrator\ph_titrator.ino')
function Need([string]$p,[string]$m) { if ($sketch -notmatch $p) { throw "FAIL: $m" } }
function Reject([string]$p,[string]$m) { if ($sketch -match $p) { throw "FAIL: $m" } }
Reject "href='/action\?" 'browser actions must not use GET links'
Reject "method='get'" 'browser mutation forms must not use GET'
Reject "fetch\('/action\?" 'browser actions must not put arguments in URLs'
Reject 'X-Session-Token[''"]?\s*\+|[?&](token|session)=' 'session tokens must never be concatenated into URLs'
Need "sessionStorage\.getItem\('k10_session'\)" 'browser session must be kept in sessionStorage'
Need "headers\['X-Session-Token'\]=token" 'browser mutations must use the session header'
foreach ($id in 'loginForm','logoutButton','recoveryForm') { Need ("id='" + $id + "'") "missing browser authentication control $id" }
foreach ($id in 'loginForm','recoveryForm') {
  Reject ("<form[^>]*id='" + $id + "'") "$id must not have a native submission path"
  Need ("<div id='" + $id + "'") "$id must be an inert container"
}
Need "id='loginButton' type='button'" 'login must use an explicit non-submit button'
Need "id='recoveryButton'[^>]*type='button'" 'recovery must use an explicit non-submit button'
Need "addEventListener\('keydown'.*?e\.key==='Enter'" 'authentication Enter handling must be explicit JavaScript'
Need 'function apiPost\(path,form,allowAnonymous\)' 'browser mutations need one POST seam'
Need "fetch\(path,\{method:'POST',headers,body:new URLSearchParams\(form\)\}\)" 'apiPost must form-encode POST bodies'
Need "response\.status===401[\s\S]*?sessionStorage\.removeItem\('k10_session'\)[\s\S]*?showLogin\(\)" '401 must clear the session and show login'
Need "response\.status===403" '403 must be handled without clearing the session'
Need "response\.status===429" '429 must show a generic lockout message'
Need "function apiPost[\s\S]*?if\(response\.status===429\)throw new Error\('Too many attempts\. Try again later\.'\)[\s\S]*?if\(!response\.ok\)" 'apiPost must replace server 429 detail before general server errors'
Need 'TextEncoder\(\).*?\.encode\(.*?\)\.length|new TextEncoder\(\)\.encode\(.*?\)\.length' 'recovery password validation must count UTF-8 bytes'
Need "apiPost\('/panic',\{\},true\)" 'emergency stop UI must use the dedicated anonymous endpoint'
Reject 'data-cmd=''panic''|cmd == "panic"' 'panic must never map through the general action handler'
Need "fetch\('/ota',\{method:'POST',headers:headers,body:fd\}\)" 'OTA must POST FormData with token headers'
Reject "fetch\('/ota'[\s\S]{0,200}Content-Type" 'OTA must not set Content-Type manually'
Need "fetch\('/ota'[\s\S]*?if\(response\.status===429\)throw new Error\('Too many attempts\. Try again later\.'\)[\s\S]*?if\(!response\.ok\)" 'OTA must replace server 429 detail before general server errors'
Need 'server\.on\("/action",\s*HTTP_POST,\s*handleAction\)' '/action must be POST'
Need 'server\.on\("/set",\s*HTTP_POST,\s*handleSet\)' '/set must be POST'
Need 'server\.on\("/action",\s*HTTP_GET,\s*handleMethodNotAllowed\)' 'GET /action must be 405'
Need 'server\.on\("/set",\s*HTTP_GET,\s*handleMethodNotAllowed\)' 'GET /set must be 405'
foreach ($route in 'login','logout','recover') { Need ('server\.on\("/' + $route + '",\s*HTTP_POST') "/$route must be POST-only" }
Need 'void handleSet\(\) \{\s*uint8_t sessionSlot;\s*if \(!requireCommand\(WebCommand::SaveMethodSettings, sessionSlot\)\)' '/set must authenticate/admit before mutation'
Need 'void handleAction\(\) \{[\s\S]*?String cmd = server\.arg\("cmd"\);[\s\S]*?requireCommand\(' '/action must parse then authenticate/admit'
Need 'UPLOAD_FILE_START[\s\S]*?validateSession[\s\S]*?admitWebCommand\(WebCommand::OtaUpload[\s\S]*?enterHttpOtaSafety\(\)[\s\S]*?Update\.begin' 'OTA authentication/admission must precede safety and Update.begin'
Need 'void handleRecover\(\) \{\s*pump\.stop\(\);\s*samplePump\.stop\(\);[\s\S]*?authManager\.recover[\s\S]*?resetRunData\(\);[\s\S]*?RunState::SetupMode' 'recovery ordering is unsafe'
Need 'void handlePanic\(\) \{\s*uint8_t sessionSlot;\s*if \(!requireCommand\(WebCommand::EmergencyStop, sessionSlot\)\) return;\s*pump\.stop\(\);\s*samplePump\.stop\(\);' 'anonymous panic must pass centralized emergency admission before stop-only action'
Need 'bool otaUploadStartSeen = false;' 'OTA lifecycle must track a current start event'
Need 'void clearOtaRequestState\(\)' 'OTA lifecycle needs one explicit clearing operation'
Need 'void handleOta\(\) \{\s*if \(!otaUploadStartSeen \|\| !otaRequestAccepted\)' 'empty/malformed OTA requests must be rejected'
Need 'void handleOta\(\) \{[\s\S]*?clearOtaRequestState\(\);\s*\}' 'OTA final handler must clear request state'
Need 'UPLOAD_FILE_START\) \{\s*clearOtaRequestState\(\);\s*otaUploadStartSeen = true;' 'each OTA start must reset stale authorization'
Need 'UPLOAD_FILE_ABORTED\) \{[\s\S]*?clearOtaRequestState\(\);' 'OTA abort must clear request state'
Need 'struct SettingsCandidate[\s\S]*?SettingsCandidate candidate =' '/set must stage request values'
Need 'if \(calibrationChanged && !validateSettingsCandidate\(candidate' '/set must only validate calibration spans when calibration fields changed'
Need 'commitSettingsCandidate\(candidate\);[\s\S]*?recordSuccessfulWrite' '/set must commit before refreshing session'
Need 'resetFromHttpOtaFailure\(\);\s*authManager\.recordSuccessfulWrite\(sessionSlot, millis\(\)\);' 'successful terminal OTA reset must refresh session'
$setBeforeValidation = [regex]::Match($sketch, 'void handleSet\(\) \{([\s\S]*?)if \(calibrationChanged && !validateSettingsCandidate\(candidate').Groups[1].Value
if (-not $setBeforeValidation) { throw 'FAIL: could not inspect /set staging prefix' }
if ($setBeforeValidation -cmatch 'save[A-Z]|setRunPulseUs|setCalibrationFactor|selectMethod|setState|resetRunData') {
  throw 'FAIL: /set performs a side effect before whole-request validation'
}
$rejectedOtaWrite = [regex]::Match($sketch, 'else if \(upload\.status == UPLOAD_FILE_WRITE\) \{([\s\S]*?)\n  \}').Groups[1].Value
if ($rejectedOtaWrite -notmatch '^\s*if \(!otaRequestAccepted\) return;') { throw 'FAIL: rejected OTA chunks can reach Update.write' }
$json = [regex]::Match($sketch, 'void handleJson\(\) \{([\s\S]*?)\n\}').Groups[1].Value
if ($json -match 'password|salt|hash|session.?token') { throw 'FAIL: /json exposes authentication material' }
Write-Host 'All web authentication static tests passed'
