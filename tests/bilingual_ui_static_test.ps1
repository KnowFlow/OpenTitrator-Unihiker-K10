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
Need "topTools" 'language selector must have a responsive layout hook'
Need "(?s)topTools.*@media\(max-width:720px\)" 'mobile header layout must be optimized'
Need "setupLanguage\(\);if\(sessionToken\(\)\)" 'language setup must run during page startup'

Write-Output 'Bilingual UI static tests passed'
