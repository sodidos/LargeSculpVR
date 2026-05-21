$ErrorActionPreference = "Stop"

$questRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
& (Join-Path $questRoot "check_quest_env.ps1")

$jdk = Get-ChildItem "C:\Program Files\Eclipse Adoptium" -Directory -Filter "jdk-17*" -ErrorAction SilentlyContinue |
    Sort-Object Name -Descending |
    Select-Object -First 1
if ($jdk) {
    $env:JAVA_HOME = $jdk.FullName
    $env:PATH = "$($jdk.FullName)\bin;$env:PATH"
}

$sdkRoot = "E:\Android\Sdk"
if (Test-Path -LiteralPath $sdkRoot) {
    $env:ANDROID_HOME = $sdkRoot
    $env:ANDROID_SDK_ROOT = $sdkRoot
    $env:ANDROID_NDK_HOME = Join-Path $sdkRoot "ndk\27.0.12077973"
    $env:ANDROID_NDK_ROOT = $env:ANDROID_NDK_HOME
    $env:PATH = "$(Join-Path $sdkRoot 'platform-tools');$(Join-Path $sdkRoot 'cmdline-tools\latest\bin');$env:PATH"
}

$gradlew = Join-Path $questRoot "gradlew.bat"
if (Test-Path -LiteralPath $gradlew) {
    $gradleCommand = $gradlew
} elseif (Test-Path -LiteralPath "E:\Tools\gradle-8.10.2\bin\gradle.bat") {
    $gradleCommand = "E:\Tools\gradle-8.10.2\bin\gradle.bat"
} else {
    $gradle = Get-Command gradle -ErrorAction Stop
    $gradleCommand = $gradle.Source
}

Push-Location $questRoot
try {
    & $gradleCommand ":app:assembleDebug"
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
} finally {
    Pop-Location
}
