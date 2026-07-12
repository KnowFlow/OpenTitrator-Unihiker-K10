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
$json = [regex]::Match($sketch, 'void handleJson\(\) \{([\s\S]*?)\n\}').Groups[1].Value
if ($json -match 'password|salt|hash|session.?token') { throw 'FAIL: /json exposes authentication material' }
Write-Host 'All web authentication static tests passed'
