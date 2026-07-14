param(
    [Parameter(Mandatory=$true)][string]$Bin,
    [Parameter(Mandatory=$true)][string]$Password,
    [string]$Ip = "192.168.9.42",
    [scriptblock]$Transport
)

$uri = "http://$Ip/ota"
Write-Host "Uploading $Bin to $uri ..."

$client = $null
$stream = $null
$multipart = $null
$request = $null
try {
    $stream = [System.IO.File]::OpenRead((Resolve-Path -LiteralPath $Bin))
    $multipart = [System.Net.Http.MultipartFormDataContent]::new()
    $fileContent = [System.Net.Http.StreamContent]::new($stream)
    $fileContent.Headers.ContentType = [System.Net.Http.Headers.MediaTypeHeaderValue]::new("application/octet-stream")
    $disposition = [System.Net.Http.Headers.ContentDispositionHeaderValue]::new("form-data")
    $disposition.Name = '"file"'
    $disposition.FileName = '"' + [System.IO.Path]::GetFileName($Bin).Replace('"', '') + '"'
    $fileContent.Headers.ContentDisposition = $disposition
    $multipart.Add($fileContent)
    $request = [System.Net.Http.HttpRequestMessage]::new([System.Net.Http.HttpMethod]::Post, $uri)
    [void]$request.Headers.TryAddWithoutValidation("X-OTA-Password", $Password)
    $request.Content = $multipart
    if ($Transport) {
        $result = & $Transport $request
    } else {
        $client = [System.Net.Http.HttpClient]::new()
        $client.Timeout = [TimeSpan]::FromSeconds(60)
        $response = $client.SendAsync($request).GetAwaiter().GetResult()
        $response.EnsureSuccessStatusCode() | Out-Null
        $result = $response.Content.ReadAsStringAsync().GetAwaiter().GetResult()
    }
    Write-Host "Response: $result"
    if ($result -ne "OK") { Write-Host "OTA upload failed."; exit 1 }
    Write-Host "OTA upload successful. Device will restart in ~1.2s."
} catch {
    Write-Host "Error: OTA request failed."
    exit 1
} finally {
    if ($request) { $request.Dispose() }
    elseif ($multipart) { $multipart.Dispose() }
    if ($stream) { $stream.Dispose() }
    if ($client) { $client.Dispose() }
}
