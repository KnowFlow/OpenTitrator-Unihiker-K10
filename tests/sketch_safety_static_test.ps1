$ErrorActionPreference = 'Stop'

$sketchPath = Join-Path $PSScriptRoot '..\ph_titrator\ph_titrator.ino'
$sketch = Get-Content -Raw $sketchPath

function Assert-Match([string]$Pattern, [string]$Message) {
  if ($sketch -notmatch $Pattern) {
    throw "FAIL: $Message"
  }
}

Assert-Match 'void setState\(RunState next, const String &status\) \{\s*if \(next == RunState::Error\) \{\s*endpointHold\.reset\(\);' 'every Error transition resets endpoint hold'
Assert-Match 'void handleSet\(\) \{\s*uint8_t sessionSlot;\s*if \(!requireCommand\(WebCommand::SaveMethodSettings, sessionSlot\)\)' 'authentication and admission reject /set before parsing fields'
Assert-Match 'if \(methodChanged \|\| methodFieldChanged \|\| endpointChanged\) \{\s*endpointHold\.reset\(\);\s*\}' 'method, endpoint, or control workflow changes reset endpoint hold'
Assert-Match 'void handleButton\(ButtonEvent event\) \{[\s\S]*?if \(httpOtaSafetyLock\) \{\s*if \(event == ButtonEvent::ABLong\) \{\s*pump\.stop\(\);\s*samplePump\.stop\(\);\s*\}\s*return;' 'buttons preserve OTA state and lock while allowing emergency pump stop'
Assert-Match 'if \(cmd == "reset" && !httpOtaInProgress && !httpOtaSucceeded\)' 'Web reset clears only a terminal failed OTA lock'

$PythonUploader = Get-Content (Join-Path $PSScriptRoot '..\scripts\ota_upload.py') -Raw
if ($PythonUploader -notmatch 'headers=\{[\s\S]*?"X-Session-Token": token') { throw 'FAIL: Python OTA uploader must send X-Session-Token' }
if ($PythonUploader -notmatch 'add_argument\("--token", required=True') { throw 'FAIL: Python OTA uploader must require --token' }
$PowerShellUploader = Get-Content (Join-Path $PSScriptRoot '..\scripts\ota_upload.ps1') -Raw
if ($PowerShellUploader -notmatch '\[Parameter\(Mandatory=\$true\)\][\s\S]*?\[string\]\$Token') { throw 'FAIL: PowerShell OTA uploader must require -Token' }
if ($PowerShellUploader -notmatch 'TryAddWithoutValidation\("X-Session-Token",\s*\$Token\)') { throw 'FAIL: PowerShell OTA uploader must send X-Session-Token' }
if ($PowerShellUploader -notmatch 'MultipartFormDataContent' -or $PowerShellUploader -notmatch '\.Name\s*=\s*''"file"''') { throw 'FAIL: PowerShell OTA uploader must send multipart file field' }

$start = [regex]::Match($sketch, 'bool startTitration\(\) \{([\s\S]*?)\n\}')
if (-not $start.Success) { throw 'FAIL: startTitration not found' }
$resetCount = ([regex]::Matches($start.Groups[1].Value, 'endpointHold\.reset\(\);')).Count
if ($resetCount -ne 1) { throw "FAIL: startTitration must reset endpoint hold exactly once; got $resetCount" }

Write-Host 'All sketch safety static tests passed'
