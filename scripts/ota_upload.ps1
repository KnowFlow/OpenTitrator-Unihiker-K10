param(
    [Parameter(Mandatory=$true)]
    [string]$Bin,
    [Parameter(Mandatory=$true)]
    [string]$Token,
    [string]$Ip = "192.168.9.42"
)

$uri = "http://$Ip/ota"
Write-Host "Uploading $Bin to $uri ..."

try {
    $headers = @{ "X-Session-Token" = $Token }
    $response = Invoke-WebRequest -Uri $uri -Method POST -Headers $headers -InFile $Bin -ContentType "application/octet-stream" -TimeoutSec 60
    $result = $response.Content
    Write-Host "Response: $result"
    if ($result -eq "OK") {
        Write-Host "OTA upload successful. Device will restart in ~1.2s."
    } else {
        Write-Host "OTA upload failed."
        exit 1
    }
} catch {
    Write-Host "Error: $_"
    exit 1
}
