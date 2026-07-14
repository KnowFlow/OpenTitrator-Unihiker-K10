$ErrorActionPreference = 'Stop'

$sketchPath = Join-Path $PSScriptRoot '..\ph_titrator\ph_titrator.ino'
$page = Get-Content -Raw (Join-Path $PSScriptRoot '..\ph_titrator\web_ui_page.inc')
$script = Get-Content -Raw (Join-Path $PSScriptRoot '..\ph_titrator\web_ui_script.inc')
$sketch = (Get-Content -Raw $sketchPath) + $page.Replace('#include "web_ui_script.inc"', $script)

function Assert-Match([string]$Pattern, [string]$Message) {
  if ($sketch -notmatch $Pattern) {
    throw "FAIL: $Message"
  }
}

Assert-Match 'void handleSet\(\) \{\s*uint8_t sessionSlot;\s*if \(!requireCommand\(WebCommand::SaveMethodSettings, sessionSlot\)\)' 'authentication and admission reject /set before parsing fields'
Assert-Match 'void handleButton\(ButtonEvent event\) \{[\s\S]*?if \(httpOtaSafetyLock\) \{\s*if \(event == ButtonEvent::ABLong\) \{\s*pump\.stop\(\);\s*samplePump\.stop\(\);\s*dispatchRunCommand\(RunCommand::EmergencyStop\);\s*\}\s*return;' 'buttons preserve OTA state and lock while synchronizing an emergency stop'
Assert-Match 'if \(cmd == "reset" && !httpOtaInProgress && !httpOtaSucceeded\)' 'Web reset clears only a terminal failed OTA lock'

$PythonUploader = Get-Content (Join-Path $PSScriptRoot '..\scripts\ota_upload.py') -Raw
if ($PythonUploader -notmatch 'headers=\{[\s\S]*?"X-OTA-Password": password') { throw 'FAIL: Python OTA uploader must send X-OTA-Password' }
if ($PythonUploader -notmatch 'add_argument\("--password", required=True') { throw 'FAIL: Python OTA uploader must require --password' }
$PowerShellUploader = Get-Content (Join-Path $PSScriptRoot '..\scripts\ota_upload.ps1') -Raw
if ($PowerShellUploader -notmatch '\[Parameter\(Mandatory=\$true\)\][\s\S]*?\[string\]\$Password') { throw 'FAIL: PowerShell OTA uploader must require -Password' }
if ($PowerShellUploader -notmatch 'TryAddWithoutValidation\("X-OTA-Password",\s*\$Password\)') { throw 'FAIL: PowerShell OTA uploader must send X-OTA-Password' }
if ($PowerShellUploader -notmatch 'MultipartFormDataContent' -or $PowerShellUploader -notmatch '\.Name\s*=\s*''"file"''') { throw 'FAIL: PowerShell OTA uploader must send multipart file field' }

Assert-Match '#include "run_engine\.h"' 'sketch includes the pure RunEngine boundary'
Assert-Match 'RunEngine runEngine;' 'sketch owns exactly one RunEngine instance'
Assert-Match 'RunInput buildRunInput\(RunCommand command\)' 'adapter snapshots facts into RunInput'
Assert-Match 'void applyPumpIntent\(PumpController &target, const PumpIntent &intent, bool titrant\)' 'adapter owns pump intent translation'
Assert-Match 'void applyRunOutput\(const RunOutput &output\)' 'adapter maps engine output to display state'
Assert-Match 'String runStatusText\(RunStatusCode status\)' 'adapter maps engine status text'
Assert-Match 'ReStabilizingAfterResume[\s\S]*?正在重新稳定信号' 'resume re-stabilization has a user-facing status'
Assert-Match 'void dispatchRunCommand\(RunCommand command\)' 'all active entry points use one command helper'
Assert-Match 'dispatchRunCommand\(RunCommand::EmergencyStop\)' 'hardware and Web emergency stops synchronize the RunEngine'
Assert-Match 'dispatchRunCommand\(RunCommand::StartNormal\)' 'normal starts use StartNormal'
Assert-Match 'dispatchRunCommand\(RunCommand::StartExistingSample\)' 'existing sample starts use StartExistingSample'
Assert-Match 'dispatchRunCommand\(RunCommand::Pause\)' 'pauses use Pause'
Assert-Match 'dispatchRunCommand\(RunCommand::Resume\)' 'resumes use Resume'
Assert-Match 'dispatchRunCommand\(RunCommand::Reset\)' 'resets use Reset'
Assert-Match 'dispatchRunCommand\(RunCommand::Tick\)' 'main loop uses Tick'
Assert-Match 'RunTelemetry telemetry = runEngine\.telemetry\(\);' 'JSON reads its diagnostics from the engine'
Assert-Match 'String\(telemetry\.predoseTargetGrams, 2\)' 'JSON exposes engine-owned predose telemetry'
Assert-Match 'String\(telemetry\.eqpPointCount\)' 'JSON exposes engine-owned EQP telemetry'

Assert-Match "id='recordSampleId'" 'record sample metadata field exists'
Assert-Match "id='recordBatchReference'" 'record batch/reference metadata field exists'
Assert-Match "id='recordOperator'" 'record operator metadata field exists'
Assert-Match "id='recordNotes'" 'record notes metadata field exists'
Assert-Match 'function newRunRecord\(\)' 'record initializer exists'
Assert-Match 'function observeRunRecord\(d\)' 'record observer exists'
Assert-Match 'function renderRunRecord\(\)' 'record renderer exists'
Assert-Match 'runRecord\.confirmed=completed' 'aborted records remain unconfirmed'
Assert-Match 'function recordPoint\(\)\{var p=recordCopy\(curve\[curve\.length-1\]\);p\.elapsed_s=runRecord\.points\.length\?\(new Date\(p\.ts\)-new Date\(runRecord\.startedAt\)\)/1000:0;return p\}' 'record points use a record-start-relative elapsed timeline with a zero first point'
Assert-Match 'runRecord\.points=d\.adc_ok&&curve\.length\?\[recordPoint\(\)\]:\[\]' 'first active telemetry starts empty or with its normalized current curve point'
Assert-Match 'else if\(runRecord&&runRecord\.startedAt&&!runRecord\.final&&d\.adc_ok&&curve\.length\)\{runRecord\.points\.push\(recordPoint\(\)\)\}' 'later telemetry appends normalized copies only while the record is not finalized'
Assert-Match 'function recordDeviceSnapshot\(d\)\{return\{network:d\.network,ip:d\.ip,ap_ip:d\.ap_ip,sta_ip:d\.sta_ip,sta_connected:d\.sta_connected,ota:d\.ota,ota_safety_lock:d\.ota_safety_lock,status:d\.status,' 'device snapshots retain firmware-independent network and status facts'
$recordControl = [regex]::Match($sketch, 'var runRecord=null(?:,recordDb=null)?;[\s\S]*?(?=page \+= F\("async function poll)').Value
if (-not $recordControl) { throw 'FAIL: could not inspect browser record control path' }
if ($recordControl -match '/action|/set|/ota|apiPost') { throw 'FAIL: record control path must not control device' }
Assert-Match 'recordCurve\(d\);[\s\S]*?observeRunRecord\(d\);' 'record observer runs after curve recorder'
Assert-Match 'function exportRunRecord\(\)' 'record JSON exporter exists'
Assert-Match 'function printRunReport\(\)' 'print report function exists'
Assert-Match "JSON\.stringify\(runRecord\.deviceSnapshot\|\|\{\},null,2\)" 'print report includes every device snapshot field'
Assert-Match 'ABORTED / NOT CONFIRMED' 'aborted reports are visibly marked'
Assert-Match 'confirmed' 'report stores confirmation state'

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
