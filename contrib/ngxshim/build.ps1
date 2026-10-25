# One-command build + install of the optional DLSS shim for Slipstream (Windows, x64).
# SPDX-License-Identifier: MIT -- see LICENSE.
#
#   powershell -ExecutionPolicy Bypass -File contrib\ngxshim\build.ps1 C:\src\DLSS [OutputDir] [-StaticCrt]
#
# Runs ONLY on your machine, against YOUR OWN clone of https://github.com/NVIDIA/DLSS (you
# accept NVIDIA's licence by downloading it). It is never run by CI and never packaged into a
# viewer build. It checks the prerequisites (CMake, Visual Studio 2022 or 2026 with the C++ workload,
# the Vulkan SDK, the SDK files), builds the shim (Release) in a temporary folder, and copies
# slipstream_ngx.dll plus your SDK's runtime DLLs (lib\Windows_x86_64\rel) into OutputDir
# (default: %APPDATA%\SlipstreamViewer\dlss, a folder the viewer looks in by default) via
# temp file + rename, so a running viewer's loaded DLLs are never overwritten in place
# (Windows would refuse; close the viewer if a file is locked and run again).
param(
    [Parameter(Mandatory = $true, Position = 0)][string]$SdkDir,
    [Parameter(Position = 1)][string]$OutputDir = '',
    [switch]$StaticCrt
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Fail([string]$msg) { Write-Host "error: $msg" -ForegroundColor Red; exit 1 }
function Note([string]$msg) { Write-Host "==> $msg" }

$ShimSrc = Split-Path -Parent $MyInvocation.MyCommand.Path
$DefaultOut = Join-Path $env:APPDATA 'SlipstreamViewer\dlss'
if ($env:SLIPSTREAMVIEWER_USER_DIR) { $DefaultOut = Join-Path $env:SLIPSTREAMVIEWER_USER_DIR 'dlss' }
if (-not $OutputDir) { $OutputDir = $DefaultOut }

# ---- prerequisites (nothing is written until all of these pass) ----
if (-not (Test-Path -LiteralPath $SdkDir -PathType Container)) {
    Fail "SDK folder not found: $SdkDir`n  Clone it first:  git clone https://github.com/NVIDIA/DLSS"
}
$SdkDir = (Resolve-Path -LiteralPath $SdkDir).Path
if (-not (Test-Path (Join-Path $SdkDir 'include\nvsdk_ngx_vk.h'))) {
    Fail "$SdkDir is not the NVIDIA DLSS SDK (missing include\nvsdk_ngx_vk.h)"
}
$Plat = Join-Path $SdkDir 'lib\Windows_x86_64'
$NgxLibName = if ($StaticCrt) { 'nvsdk_ngx_s.lib' } else { 'nvsdk_ngx_d.lib' }
if (-not ((Test-Path (Join-Path $Plat "x64\$NgxLibName")) -or (Test-Path (Join-Path $Plat $NgxLibName)))) {
    Fail "NGX loader library missing: $Plat\x64\$NgxLibName (the SDK clone is incomplete; try 'git lfs pull')"
}
$RtLibs = @('nvngx_dlss.dll', 'nvngx_dlssd.dll') | Where-Object { Test-Path (Join-Path $Plat "rel\$_") }
if (-not $RtLibs) { Fail "no DLSS runtime DLLs in $Plat\rel (expected nvngx_dlss.dll and nvngx_dlssd.dll; try 'git lfs pull')" }
if ($RtLibs -notcontains 'nvngx_dlss.dll')  { Write-Warning 'no nvngx_dlss.dll (Super Resolution) in the SDK; only Ray Reconstruction will work' }
if ($RtLibs -notcontains 'nvngx_dlssd.dll') { Write-Warning 'no nvngx_dlssd.dll (Ray Reconstruction) in the SDK; only Super Resolution will work' }

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    Fail 'cmake not found (need 3.16 or newer; install CMake and tick "add to PATH", or use the one bundled with Visual Studio)'
}
# Visual Studio 2022 (17.x) or 2026 (18.x) with the C++ workload (vswhere ships with the installer).
# The newest one installed is used; its CMake generator follows its major version.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { Fail 'Visual Studio not found. Install Visual Studio 2022 or 2026 with the "Desktop development with C++" workload.' }
$vsVer = & $vswhere -latest -version '[17.0,19.0)' -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion
if (-not $vsVer) { Fail 'Visual Studio 2022 or 2026 with the "Desktop development with C++" workload (MSVC x64 tools) was not found.' }
$vsMajor = [int]($vsVer.Split('.')[0])
$Toolset = @()
if ($vsMajor -ge 18) {
    $Generator = 'Visual Studio 18 2026'
    # CMake learned the VS 2026 generator in 4.2: older CMakes reject it.
    $gens = (& cmake --help) -join "`n"
    if ($gens -notmatch 'Visual Studio 18 2026') { Fail 'Visual Studio 2026 needs CMake 4.2 or newer (this CMake has no "Visual Studio 18 2026" generator). Update CMake from https://cmake.org/download/ and open a new terminal.' }
    # NVIDIA's static NGX library is built with the VS 2022 toolset (v143): use it when installed
    # (Visual Studio Installer > Individual components > "MSVC v143 - VS 2022 C++ x64/x86 build tools").
    $v143 = & $vswhere -latest -version '[18.0,19.0)' -products * -requires Microsoft.VisualStudio.Component.VC.v143.x86.x64 -property installationPath
    if ($v143) { $Toolset = @('-T', 'v143') }
    else { Write-Warning 'Visual Studio 2026 without the v143 (VS 2022) build tools: building with the default toolset. If linking fails, add "MSVC v143 - VS 2022 C++ x64/x86 build tools" in the Visual Studio Installer and run again.' }
} else {
    $Generator = 'Visual Studio 17 2022'
}
# Vulkan headers: the Vulkan SDK (sets VULKAN_SDK). CMake's FindVulkan uses it.
if (-not $env:VULKAN_SDK -or -not (Test-Path (Join-Path $env:VULKAN_SDK 'Include\vulkan\vulkan.h'))) {
    Fail 'Vulkan SDK not found (VULKAN_SDK unset or has no Include\vulkan\vulkan.h). Install it from https://vulkan.lunarg.com/ and open a new terminal.'
}

# ---- build in a temp dir ----
$Work = Join-Path ([IO.Path]::GetTempPath()) ("slipstream-ngxshim-" + [Guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $Work | Out-Null
try {
    $crt = if ($StaticCrt) { 'ON' } else { 'OFF' }
    Note "Configuring (Release, $Generator x64$(if ($Toolset) { ', toolset v143' }))"
    & cmake -S $ShimSrc -B (Join-Path $Work 'build') -G $Generator -A x64 @Toolset "-DDLSS_SDK_DIR=$SdkDir" "-DSLIP_NR_STATIC_CRT=$crt" *> (Join-Path $Work 'configure.log')
    if ($LASTEXITCODE -ne 0) { Get-Content (Join-Path $Work 'configure.log') -Tail 30; Fail 'cmake configure failed (log above)' }
    Note 'Building'
    & cmake --build (Join-Path $Work 'build') --config Release --parallel *> (Join-Path $Work 'build.log')
    if ($LASTEXITCODE -ne 0) { Get-Content (Join-Path $Work 'build.log') -Tail 40; Fail 'build failed (log above)' }
    $Shim = Join-Path $Work 'build\Release\slipstream_ngx.dll'
    if (-not (Test-Path $Shim)) { Fail "build finished but $Shim is missing" }

    # ---- install: temp file in the target folder, then rename over the old file ----
    New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
    function Install-One([string]$src, [string]$name) {
        $dest = Join-Path $OutputDir $name
        $tmp = Join-Path $OutputDir (".$name." + [Guid]::NewGuid().ToString('N').Substring(0, 8) + '.tmp')
        try {
            Copy-Item -LiteralPath $src -Destination $tmp -Force
            Move-Item -LiteralPath $tmp -Destination $dest -Force
        } catch {
            Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
            Fail "could not install $name into $OutputDir ($($_.Exception.Message)). If the viewer is running, close it and run this again."
        }
        Write-Host "    $dest"
    }
    Note "Installing into $OutputDir"
    Install-One $Shim 'slipstream_ngx.dll'
    foreach ($n in $RtLibs) { Install-One (Join-Path $Plat "rel\$n") $n }
}
finally {
    Remove-Item -LiteralPath $Work -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ''
Note "Done. Restart the viewer; Slipstream.log will have a 'DLSS:' line."
if ((Resolve-Path -LiteralPath $OutputDir).Path.TrimEnd('\') -ieq $DefaultOut.TrimEnd('\')) {
    Write-Host 'The viewer looks in this folder by default: no setting is needed (RenderVKDlssShimPath stays empty).'
} else {
    Write-Host 'Not the default folder. In the viewer, set the debug setting:'
    Write-Host "    RenderVKDlssShimPath = $OutputDir"
}
