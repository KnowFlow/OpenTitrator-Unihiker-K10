$ErrorActionPreference = 'Stop'
$Uploader = Join-Path $PSScriptRoot '..\scripts\ota_upload.ps1'
$Firmware = New-TemporaryFile
try {
    [IO.File]::WriteAllBytes($Firmware, [byte[]](0, 1, 2, 255, 65, 66))
    $missing = & pwsh -NoProfile -File $Uploader -Bin $Firmware 2>&1 | Out-String
    if ($LASTEXITCODE -eq 0 -or $missing -notmatch 'Password') { throw 'FAIL: PowerShell uploader accepts a missing password' }

    $transport = {
      param($request)
      if (($request.Headers.GetValues('X-OTA-Password') | Select-Object -First 1) -ne 'secret-password') { throw 'missing header' }
      $contentType = $request.Content.Headers.ContentType.ToString()
      if ($contentType -notmatch '^multipart/form-data; boundary="?([^";]+)') { throw 'not multipart' }
      $boundary = $Matches[1]
      $bytes = $request.Content.ReadAsByteArrayAsync().GetAwaiter().GetResult()
      $body = [Text.Encoding]::Latin1.GetString($bytes)
      if ($body -notmatch [regex]::Escape('--' + $boundary)) { throw 'boundary absent from body' }
      if ($body -notmatch 'Content-Disposition: form-data; name="file"; filename="[^"\r\n]+"') { throw 'file disposition absent' }
      $needle = [byte[]](0, 1, 2, 255, 65, 66)
      $found = $false
      for ($i=0; $i -le $bytes.Length-$needle.Length; $i++) {
        $match=$true; for ($j=0; $j -lt $needle.Length; $j++) { if ($bytes[$i+$j] -ne $needle[$j]) {$match=$false; break} }
        if ($match) {$found=$true; break}
      }
      if (-not $found) { throw 'firmware bytes absent' }
      'OK'
    }
    $global:LASTEXITCODE = 0
    $success = & $Uploader -Bin $Firmware -Ip '192.0.2.1' -Password 'secret-password' -Transport $transport 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0 -or $success -match 'secret-password') { throw 'FAIL: multipart success path failed or printed password' }

    $global:LASTEXITCODE = 0
    $failure = & $Uploader -Bin $Firmware -Ip '192.0.2.1' -Password 'secret-password' -Transport { throw 'transport secret-password' } 2>&1 | Out-String
    if ($LASTEXITCODE -eq 0 -or $failure -match 'secret-password') { throw 'FAIL: error path succeeded or printed password' }
    Write-Host 'All PowerShell OTA uploader behavioral tests passed'
} finally {
    Remove-Item -LiteralPath $Firmware -Force -ErrorAction SilentlyContinue
}
exit 0
