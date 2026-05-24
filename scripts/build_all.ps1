# scripts/build_all.ps1
#
# 双架构构建入口：依次配置并编译 x86 / x64 插件。
#
# 用法：
#   pwsh scripts\build_all.ps1                 # 默认构建 x86 + x64 + Release
#   pwsh scripts\build_all.ps1 -Arch x64       # 仅构建 x64
#   pwsh scripts\build_all.ps1 -Config Debug   # Debug
#   pwsh scripts\build_all.ps1 -Clean          # 先清理 build/

[CmdletBinding()]
param(
    [ValidateSet('x86', 'x64', 'all')]
    [string] $Arch = 'all',

    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')]
    [string] $Config = 'Release',

    [switch] $Clean,

    [string] $VcpkgRoot = $(if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { 'F:\vcpkg-master\vcpkg' }),

    [string] $QtRoot = 'F:/Qt/5.12.12'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = Split-Path -Parent $PSScriptRoot
Write-Host "[build_all] repo: $repoRoot" -ForegroundColor Cyan
Write-Host "[build_all] vcpkg: $VcpkgRoot" -ForegroundColor Cyan
Write-Host "[build_all] qt:    $QtRoot" -ForegroundColor Cyan

if (-not (Test-Path -LiteralPath (Join-Path $VcpkgRoot 'scripts/buildsystems/vcpkg.cmake'))) {
    throw "找不到 vcpkg toolchain：$VcpkgRoot"
}

$archList = if ($Arch -eq 'all') { @('x86', 'x64') } else { @($Arch) }

function Invoke-OneArch {
    param(
        [Parameter(Mandatory)] [string] $TargetArch
    )

    $isX64       = $TargetArch -eq 'x64'
    $generatorA  = if ($isX64) { 'x64' } else { 'Win32' }
    $triplet     = if ($isX64) { 'x64-windows-static-md' } else { 'x86-windows-static-md' }
    $buildDir    = Join-Path $repoRoot ("build-$TargetArch")
    $toolchain   = Join-Path $VcpkgRoot 'scripts/buildsystems/vcpkg.cmake'

    if ($Clean -and (Test-Path -LiteralPath $buildDir)) {
        Write-Host "[build_all][$TargetArch] 清理 $buildDir" -ForegroundColor Yellow
        Remove-Item -LiteralPath $buildDir -Recurse -Force
    }
    if (-not (Test-Path -LiteralPath $buildDir)) {
        New-Item -ItemType Directory -Path $buildDir | Out-Null
    }

    Write-Host "[build_all][$TargetArch] CMake configure ($generatorA, $triplet)" -ForegroundColor Green
    & cmake -S $repoRoot -B $buildDir `
        -G 'Visual Studio 17 2022' -A $generatorA `
        "-DCMAKE_TOOLCHAIN_FILE=$toolchain" `
        "-DVCPKG_TARGET_TRIPLET=$triplet" `
        "-DVCPKG_HOST_TRIPLET=x64-windows-static-md" `
        "-DQT5_ROOT=$QtRoot"
    if ($LASTEXITCODE -ne 0) { throw "CMake configure 失败 ($TargetArch)" }

    Write-Host "[build_all][$TargetArch] CMake build $Config" -ForegroundColor Green
    & cmake --build $buildDir --config $Config --parallel
    if ($LASTEXITCODE -ne 0) { throw "CMake build 失败 ($TargetArch)" }

    $ext       = if ($isX64) { 'dp64' } else { 'dp32' }
    $outputDir = Join-Path $buildDir "bin/$Config"
    $artifact  = Join-Path $outputDir "x64dbg_ai_plugin.$ext"
    if (Test-Path -LiteralPath $artifact) {
        Write-Host "[build_all][$TargetArch] 产出: $artifact" -ForegroundColor Green
    } else {
        Write-Warning "[build_all][$TargetArch] 未找到 $artifact，请检查输出目录 $outputDir"
    }
}

foreach ($a in $archList) {
    Invoke-OneArch -TargetArch $a
}

Write-Host "[build_all] 全部完成" -ForegroundColor Cyan
