$ErrorActionPreference = 'Stop'

$page = Get-Content -Raw (Join-Path $PSScriptRoot '..\ph_titrator\web_ui_page.inc')
$script = Get-Content -Raw (Join-Path $PSScriptRoot '..\ph_titrator\web_ui_script.inc')
$shell = Get-Content -Raw (Join-Path $PSScriptRoot '..\ph_titrator\web_ui_escape.cpp')
$ui = $page + $script + $shell

function Need([string]$pattern, [string]$message) {
  if ($ui -notmatch $pattern) { throw "FAIL: $message" }
}

Need "id='languageSelect'" 'language selector must be rendered'
Need "k10_language" 'selected language must persist locally'
Need "function translatePage\(\)" 'static page content must be translated after a language change'
Need "'Controller login':'控制器登录'" 'authentication UI must have Chinese coverage'
Need "'Emergency stop':'紧急停止'" 'safety UI must have Chinese coverage'
Need "'Start current sample':'开始当前样品'" 'start-existing action must have Chinese coverage'
Need "'Save WiFi':'保存 WiFi'" 'administration actions must have Chinese coverage'
Need "'Sweep sample':'扫描样品泵'" 'manual actions must have Chinese coverage'
Need "networkBar" 'network status must have a dedicated layout hook'
Need "选择已保存记录" 'saved-record selector must be translated'
Need "saved records in this browser" 'dynamic saved-record status must be translated'
Need "EQP 至少需要 3 个加液变化点" 'dynamic EQP guidance must be translated'
Need "(?s)networkBar.*@media\(max-width:720px\)" 'mobile header layout must be optimized'
Need "setupLanguage\(\);if\(sessionToken\(\)\)" 'language setup must run during page startup'
Need "https://github\.com/KnowFlow/OpenTitrator-Unihiker-K10" 'source link must target the standalone K10 repository'
Need "async function waitForOtaRestart\(\)" 'OTA feedback must wait for the controller to return'
Need "Firmware accepted. Waiting for device restart" 'OTA feedback must distinguish acceptance from restart completion'
Need "Device restarted. Log in again" 'OTA feedback must confirm reconnection and request a fresh login'
Need "var raw=await response\.text\(\)" 'OTA feedback must accept the endpoint text response'

$otaHandler = [regex]::Match($ui, 'document\.getElementById\(''otaForm''\)[^\r\n]+').Value
if ($otaHandler -match 'response\.json\(\)') { throw 'FAIL: OTA handler must not parse the text response as JSON' }

Write-Output 'Bilingual UI static tests passed'
