$ErrorActionPreference = 'Stop'
$Uploader = Join-Path $PSScriptRoot '..\scripts\ota_upload.ps1'
$Firmware = New-TemporaryFile
try {
    $missing = & pwsh -NoProfile -File $Uploader -Bin $Firmware 2>&1 | Out-String
    if ($LASTEXITCODE -eq 0 -or $missing -notmatch 'Token') { throw 'FAIL: PowerShell uploader accepts a missing token' }

    $successCommand = @"
function Invoke-WebRequest { param(`$Uri, `$Method, `$Headers, `$InFile, `$ContentType, `$TimeoutSec); if (`$Headers['X-Session-Token'] -ne 'secret-token') { throw 'missing header' }; [pscustomobject]@{Content='OK'} }
& '$Uploader' -Bin '$Firmware' -Ip '192.0.2.1' -Token 'secret-token'
"@
    $success = & pwsh -NoProfile -Command $successCommand 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0 -or $success -match 'secret-token') { throw 'FAIL: PowerShell success path omitted header or printed token' }

    $errorCommand = @"
function Invoke-WebRequest { throw 'transport secret-token' }
& '$Uploader' -Bin '$Firmware' -Ip '192.0.2.1' -Token 'secret-token'
"@
    $failure = & pwsh -NoProfile -Command $errorCommand 2>&1 | Out-String
    if ($LASTEXITCODE -eq 0 -or $failure -match 'secret-token') { throw 'FAIL: PowerShell error path succeeded or printed token' }
    Write-Host 'All PowerShell OTA uploader behavioral tests passed'
} finally {
    Remove-Item -LiteralPath $Firmware -Force -ErrorAction SilentlyContinue
}
