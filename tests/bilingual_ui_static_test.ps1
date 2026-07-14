$ErrorActionPreference = 'Stop'

$page = Get-Content -Raw (Join-Path $PSScriptRoot '..\ph_titrator\web_ui_page.inc')
$script = Get-Content -Raw (Join-Path $PSScriptRoot '..\ph_titrator\web_ui_script.inc')

function Need([string]$pattern, [string]$message) {
  if (($page + $script) -notmatch $pattern) { throw "FAIL: $message" }
}

Need "id='languageSelect'" 'language selector must be rendered'
Need "k10_language" 'selected language must persist locally'
Need "function translatePage\(\)" 'static page content must be translated after a language change'
Need "'Controller login':'控制器登录'" 'authentication UI must have Chinese coverage'
Need "'Emergency stop':'紧急停止'" 'safety UI must have Chinese coverage'
Need "setupLanguage\(\);if\(sessionToken\(\)\)" 'language setup must run during page startup'

Write-Output 'Bilingual UI static tests passed'
