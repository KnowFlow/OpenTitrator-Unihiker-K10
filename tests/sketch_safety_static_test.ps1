$ErrorActionPreference = 'Stop'

$sketchPath = Join-Path $PSScriptRoot '..\ph_titrator\ph_titrator.ino'
$sketch = Get-Content -Raw $sketchPath

function Assert-Match([string]$Pattern, [string]$Message) {
  if ($sketch -notmatch $Pattern) {
    throw "FAIL: $Message"
  }
}

Assert-Match 'void handleSet\(\) \{\s*uint8_t sessionSlot;\s*if \(!requireCommand\(WebCommand::SaveMethodSettings, sessionSlot\)\)' 'authentication and admission reject /set before parsing fields'
Assert-Match 'void handleButton\(ButtonEvent event\) \{[\s\S]*?if \(httpOtaSafetyLock\) \{\s*if \(event == ButtonEvent::ABLong\) \{\s*pump\.stop\(\);\s*samplePump\.stop\(\);\s*\}\s*return;' 'buttons preserve OTA state and lock while allowing emergency pump stop'
Assert-Match 'if \(cmd == "reset" && !httpOtaInProgress && !httpOtaSucceeded\)' 'Web reset clears only a terminal failed OTA lock'

$PythonUploader = Get-Content (Join-Path $PSScriptRoot '..\scripts\ota_upload.py') -Raw
if ($PythonUploader -notmatch 'headers=\{[\s\S]*?"X-Session-Token": token') { throw 'FAIL: Python OTA uploader must send X-Session-Token' }
if ($PythonUploader -notmatch 'add_argument\("--token", required=True') { throw 'FAIL: Python OTA uploader must require --token' }
$PowerShellUploader = Get-Content (Join-Path $PSScriptRoot '..\scripts\ota_upload.ps1') -Raw
if ($PowerShellUploader -notmatch '\[Parameter\(Mandatory=\$true\)\][\s\S]*?\[string\]\$Token') { throw 'FAIL: PowerShell OTA uploader must require -Token' }
if ($PowerShellUploader -notmatch 'TryAddWithoutValidation\("X-Session-Token",\s*\$Token\)') { throw 'FAIL: PowerShell OTA uploader must send X-Session-Token' }
if ($PowerShellUploader -notmatch 'MultipartFormDataContent' -or $PowerShellUploader -notmatch '\.Name\s*=\s*''"file"''') { throw 'FAIL: PowerShell OTA uploader must send multipart file field' }

Assert-Match '#include "run_engine\.h"' 'sketch includes the pure RunEngine boundary'
Assert-Match 'RunEngine runEngine;' 'sketch owns exactly one RunEngine instance'
Assert-Match 'RunInput buildRunInput\(RunCommand command\)' 'adapter snapshots facts into RunInput'
Assert-Match 'void applyPumpIntent\(PumpController &target, const PumpIntent &intent, bool titrant\)' 'adapter owns pump intent translation'
Assert-Match 'void applyRunOutput\(const RunOutput &output\)' 'adapter maps engine output to display state'
Assert-Match 'String runStatusText\(RunStatusCode status\)' 'adapter maps engine status text'
Assert-Match 'ReStabilizingAfterResume[\s\S]*?正在重新稳定信号' 'resume re-stabilization has a user-facing status'
Assert-Match 'void dispatchRunCommand\(RunCommand command\)' 'all active entry points use one command helper'
Assert-Match 'dispatchRunCommand\(RunCommand::StartNormal\)' 'normal starts use StartNormal'
Assert-Match 'dispatchRunCommand\(RunCommand::StartExistingSample\)' 'existing sample starts use StartExistingSample'
Assert-Match 'dispatchRunCommand\(RunCommand::Pause\)' 'pauses use Pause'
Assert-Match 'dispatchRunCommand\(RunCommand::Resume\)' 'resumes use Resume'
Assert-Match 'dispatchRunCommand\(RunCommand::Reset\)' 'resets use Reset'
Assert-Match 'dispatchRunCommand\(RunCommand::Tick\)' 'main loop uses Tick'

foreach ($legacy in @(
  'TitrationDynamics\s+phDynamics',
  'EqpTracker\s+eqpTracker',
  'EndpointHoldTracker\s+endpointHold',
  'uint32_t\s+runStartedMs',
  'uint32_t\s+sampleFillingStartedMs',
  'uint32_t\s+sampleLastProgressMs',
  'uint16_t\s+activePulseMs',
  'uint16_t\s+activeSettleMs',
  'bool\s+startTitration\(',
  'void\s+pauseTitration\(',
  'void\s+resumeTitration\('
)) {
  if ($sketch -match $legacy) { throw "FAIL: legacy active lifecycle remains: $legacy" }
}

Write-Host 'All sketch safety static tests passed'
