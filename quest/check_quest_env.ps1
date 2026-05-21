$ErrorActionPreference = "Continue"

$failures = New-Object System.Collections.Generic.List[string]

function Write-Check {
    param(
        [string]$Name,
        [bool]$Ok,
        [string]$Detail
    )

    $status = if ($Ok) { "OK" } else { "MISSING" }
    Write-Output ("[{0}] {1} - {2}" -f $status, $Name, $Detail)
}

function Find-CommandPath {
    param([string]$Name)
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($null -eq $command) {
        return $null
    }
    return $command.Source
}

function Add-ProcessPath {
    param([string]$Path)
    if (-not $Path -or -not (Test-Path -LiteralPath $Path)) {
        return
    }

    $parts = $env:PATH -split ';'
    if (-not ($parts | Where-Object { $_.TrimEnd('\') -ieq $Path.TrimEnd('\') })) {
        $env:PATH = "$Path;$env:PATH"
    }
}

function Find-Jdk {
    if ($env:JAVA_HOME -and (Test-Path -LiteralPath (Join-Path $env:JAVA_HOME "bin\java.exe"))) {
        return $env:JAVA_HOME
    }

    $candidates = @(
        "C:\Program Files\Eclipse Adoptium",
        "C:\Program Files\Java"
    )

    foreach ($root in $candidates) {
        if (-not (Test-Path -LiteralPath $root)) {
            continue
        }

        $jdk = Get-ChildItem -LiteralPath $root -Directory -Filter "jdk-17*" -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending |
            Select-Object -First 1
        if ($jdk) {
            return $jdk.FullName
        }
    }

    return $null
}

function Find-AndroidSdk {
    $candidates = @(
        $env:ANDROID_HOME,
        $env:ANDROID_SDK_ROOT,
        "E:\Android\Sdk",
        (Join-Path $env:LOCALAPPDATA "Android\Sdk"),
        "C:\Android\Sdk"
    ) | Where-Object { $_ -and $_.Trim().Length -gt 0 }

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    return $null
}

function Find-GradleRoot {
    $candidates = @(
        "E:\Tools\gradle-8.10.2"
    ) | Where-Object { $_ -and (Test-Path -LiteralPath $_) }

    foreach ($candidate in $candidates) {
        return (Resolve-Path -LiteralPath $candidate).Path
    }

    return $null
}

$jdkRoot = Find-Jdk
if ($jdkRoot) {
    $env:JAVA_HOME = $jdkRoot
    Add-ProcessPath (Join-Path $jdkRoot "bin")
}

$gradleRoot = Find-GradleRoot
if ($gradleRoot) {
    Add-ProcessPath (Join-Path $gradleRoot "bin")
}

$sdk = Find-AndroidSdk
if ($sdk) {
    $env:ANDROID_HOME = $sdk
    $env:ANDROID_SDK_ROOT = $sdk
    Add-ProcessPath (Join-Path $sdk "platform-tools")
    Add-ProcessPath (Join-Path $sdk "cmdline-tools\latest\bin")
}

function Find-LatestNdk {
    param([string]$SdkPath)

    $envCandidates = @($env:ANDROID_NDK_HOME, $env:ANDROID_NDK_ROOT) | Where-Object {
        $_ -and (Test-Path -LiteralPath $_)
    }

    foreach ($candidate in $envCandidates) {
        return (Resolve-Path -LiteralPath $candidate).Path
    }

    if (-not $SdkPath) {
        return $null
    }

    $ndkRoot = Join-Path $SdkPath "ndk"
    if (-not (Test-Path -LiteralPath $ndkRoot)) {
        return $null
    }

    $latest = Get-ChildItem -LiteralPath $ndkRoot -Directory |
        Sort-Object Name -Descending |
        Select-Object -First 1

    if ($null -eq $latest) {
        return $null
    }

    return $latest.FullName
}

$java = Find-CommandPath "java"
$javac = Find-CommandPath "javac"
$gradle = Find-CommandPath "gradle"
$adb = Find-CommandPath "adb"
$ndk = Find-LatestNdk $sdk

Write-Check "Java runtime" ($null -ne $java) ($(if ($java) { $java } else { "Install Android Studio or a JDK and set JAVA_HOME." }))
if (-not $java) { $failures.Add("java") }

Write-Check "Java compiler" ($null -ne $javac) ($(if ($javac) { $javac } else { "Install Android Studio or a JDK and set JAVA_HOME." }))
if (-not $javac) { $failures.Add("javac") }

Write-Check "Gradle command" ($null -ne $gradle) ($(if ($gradle) { $gradle } else { "Install Gradle or open quest/ in Android Studio." }))
if (-not $gradle) { $failures.Add("gradle") }

Write-Check "Android SDK" ($null -ne $sdk) ($(if ($sdk) { $sdk } else { "Set ANDROID_HOME or install Android Studio SDK." }))
if (-not $sdk) { $failures.Add("android-sdk") }

if ($sdk) {
    $platform32 = Test-Path -LiteralPath (Join-Path $sdk "platforms\android-32")
    Write-Check "Android SDK platform 32" $platform32 ($(if ($platform32) { "Installed" } else { "Install Android API 32 in SDK Manager." }))
    if (-not $platform32) { $failures.Add("android-32") }

    $platformTools = Test-Path -LiteralPath (Join-Path $sdk "platform-tools\adb.exe")
    Write-Check "SDK platform-tools" $platformTools ($(if ($platformTools) { Join-Path $sdk "platform-tools" } else { "Install Android SDK Platform-Tools." }))
    if (-not $platformTools) { $failures.Add("platform-tools") }

    $cmakeDir = Join-Path $sdk "cmake"
    $sdkCmake = (Test-Path -LiteralPath $cmakeDir) -and ((Get-ChildItem -LiteralPath $cmakeDir -Directory -ErrorAction SilentlyContinue | Select-Object -First 1) -ne $null)
    Write-Check "Android SDK CMake" $sdkCmake ($(if ($sdkCmake) { $cmakeDir } else { "Install CMake from SDK Tools." }))
    if (-not $sdkCmake) { $failures.Add("cmake") }
}

Write-Check "Android NDK" ($null -ne $ndk) ($(if ($ndk) { $ndk } else { "Install NDK (Side by side) in SDK Manager." }))
if (-not $ndk) { $failures.Add("ndk") }

Write-Check "adb in PATH" ($null -ne $adb) ($(if ($adb) { $adb } else { "Add SDK platform-tools to PATH." }))
if (-not $adb) { $failures.Add("adb") }

if ($adb) {
    Write-Output ""
    Write-Output "adb devices:"
    & $adb devices
}

Write-Check "OpenXR loader dependency" $true "Gradle will fetch org.khronos.openxr:openxr_loader_for_android:1.1.53."

Write-Output ""
if ($failures.Count -gt 0) {
    Write-Output ("Quest environment incomplete: {0}" -f ($failures -join ", "))
    exit 1
}

Write-Output "Quest environment looks ready."
exit 0
