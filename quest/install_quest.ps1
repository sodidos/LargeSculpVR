$ErrorActionPreference = "Stop"

$questRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
& (Join-Path $questRoot "build_quest.ps1")

$apk = Join-Path $questRoot "app\build\outputs\apk\debug\app-debug.apk"
if (-not (Test-Path -LiteralPath $apk)) {
    throw "APK not found: $apk"
}

$adb = Get-Command adb -ErrorAction Stop
& $adb.Source install -r $apk
