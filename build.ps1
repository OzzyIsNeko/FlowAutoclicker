param(
    [switch]$Clean,
    [switch]$Help
)

$ErrorActionPreference = "Stop"

if ($Help) {
    @"
Build FlowAutoclicker.exe

Usage:
  powershell -ExecutionPolicy Bypass -File .\build.ps1
  powershell -ExecutionPolicy Bypass -File .\build.ps1 -Clean

Output:
  build\FlowAutoclicker.exe
"@ | Write-Host
    exit 0
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

New-Item -ItemType Directory -Force -Path "build" | Out-Null

$targetExe = Join-Path (Resolve-Path ".").Path "build\FlowAutoclicker.exe"
$running = Get-Process FlowAutoclicker -ErrorAction SilentlyContinue | Where-Object {
    $_.Path -and ([System.StringComparer]::OrdinalIgnoreCase.Equals($_.Path, $targetExe))
}
if ($running) {
    throw "build\\FlowAutoclicker.exe is currently running. Close the app and run .\\build.ps1 again."
}

$cmdScript = @(
    "@echo off",
    "call `"$vcvars`" >nul",
    "if errorlevel 1 exit /b 1",
    "rc /nologo /fo build\app.res src\app.rc",
    "if errorlevel 1 exit /b 1",
    "cl /nologo /std:c++20 /MT /O2 /GL /Oi /Gy /Gw /GF /utf-8 /permissive- /EHsc /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /Fo:build\ /Fe:build\FlowAutoclicker.exe src\main.cpp build\app.res /link /LTCG /OPT:REF /OPT:ICF /SUBSYSTEM:WINDOWS user32.lib gdi32.lib shell32.lib ole32.lib comctl32.lib dwmapi.lib gdiplus.lib uxtheme.lib winmm.lib avrt.lib",
    "if errorlevel 1 exit /b 1"
) -join "`r`n"

Set-Content -LiteralPath "build\_build.cmd" -Value $cmdScript -Encoding ASCII
& cmd.exe /q /c "build\_build.cmd"
Remove-Item -LiteralPath "build\_build.cmd" -Force -ErrorAction SilentlyContinue
if ($LASTEXITCODE -ne 0) {
    throw "Build failed."
}

if (-not (Test-Path -LiteralPath "build\FlowAutoclicker.exe")) {
    throw "Build completed without producing build\FlowAutoclicker.exe."
}

Write-Host "Built standalone release executable at build\\FlowAutoclicker.exe"

Get-ChildItem -LiteralPath "build" -File -Filter "app.res" -ErrorAction SilentlyContinue |
    Remove-Item -Force -ErrorAction SilentlyContinue
Get-ChildItem -LiteralPath "build" -File -Filter "main.obj" -ErrorAction SilentlyContinue |
    Remove-Item -Force -ErrorAction SilentlyContinue
Get-ChildItem -LiteralPath "build" -File -Filter "main.warnings.obj" -ErrorAction SilentlyContinue |
    Remove-Item -Force -ErrorAction SilentlyContinue
Get-ChildItem -LiteralPath "build" -File -Filter "benchmark-results.txt" -ErrorAction SilentlyContinue |
    Remove-Item -Force -ErrorAction SilentlyContinue
