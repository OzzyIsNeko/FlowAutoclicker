param(
    [switch]$Clean,
    [string]$CertificateThumbprint = "",
    [string]$TimestampUrl = "http://timestamp.digicert.com",
    [string]$SignToolPath = ""
)

$ErrorActionPreference = "Stop"

function Get-SignToolPath {
    param(
        [string]$RequestedPath
    )

    if ($RequestedPath) {
        if (-not (Test-Path $RequestedPath)) {
            throw "signtool.exe was not found at $RequestedPath"
        }
        return (Resolve-Path $RequestedPath).Path
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $matches = @(& $vswhere -latest -products * -find "**\signtool.exe")
        if ($matches.Count -gt 0) {
            return $matches[0]
        }
    }

    $kitsRoot = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\bin"
    if (Test-Path $kitsRoot) {
        $match = Get-ChildItem $kitsRoot -Recurse -Filter signtool.exe -File | Sort-Object FullName -Descending | Select-Object -First 1
        if ($match) {
            return $match.FullName
        }
    }

    throw "signtool.exe was not found. Install the Windows SDK signing tools or pass -SignToolPath."
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe was not found. Install Visual Studio Build Tools with the C++ workload."
}

$installPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installPath) {
    throw "Visual C++ build tools were not found."
}

$vcvars = Join-Path $installPath "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) {
    throw "vcvars64.bat was not found at $vcvars"
}

if ($Clean -and (Test-Path "build")) {
    Remove-Item -LiteralPath "build" -Recurse -Force
}

New-Item -ItemType Directory -Force -Path "build\Release" | Out-Null

$cmdScript = @(
    "@echo off",
    "call `"$vcvars`" >nul",
    "if errorlevel 1 exit /b 1",
    "rc /nologo /fo build\Release\app.res src\app.rc",
    "if errorlevel 1 exit /b 1",
    "cl /nologo /std:c++20 /MT /O2 /GL /Oi /Gy /Gw /utf-8 /permissive- /EHsc /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /Fo:build\Release\ /Fe:build\Release\FlowAutoclicker.exe src\main.cpp build\Release\app.res /link /LTCG /OPT:REF /OPT:ICF /SUBSYSTEM:WINDOWS user32.lib gdi32.lib shell32.lib ole32.lib comctl32.lib dwmapi.lib gdiplus.lib uxtheme.lib winmm.lib",
    "if errorlevel 1 exit /b 1"
) -join "`r`n"

$null = $cmdScript | cmd.exe /q
if ($LASTEXITCODE -ne 0) {
    throw "Build failed."
}

if ($CertificateThumbprint) {
    $signTool = Get-SignToolPath -RequestedPath $SignToolPath
    & $signTool sign /fd SHA256 /sha1 $CertificateThumbprint /td SHA256 /tr $TimestampUrl "build\Release\FlowAutoclicker.exe"
    if ($LASTEXITCODE -ne 0) {
        throw "Code signing failed."
    }
}

if ($CertificateThumbprint) {
    $signature = Get-AuthenticodeSignature -FilePath "build\Release\FlowAutoclicker.exe"
    if ($signature.Status -ne "Valid") {
        throw "The release executable does not have a valid signature."
    }
    Write-Host "Built and signed standalone release executable at build\\Release\\FlowAutoclicker.exe"
} else {
    Write-Host "Built standalone release executable at build\\Release\\FlowAutoclicker.exe"
    Write-Host "Signing was skipped. Re-run with -CertificateThumbprint once your code-signing certificate is installed."
}
