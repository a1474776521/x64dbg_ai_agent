# scripts/package_release.ps1
#
# 把 build-x86 / build-x64 的插件产物 + Qt 运行依赖（如有）打包到
# release/ 目录，并生成 zip。
#
# 用法：
#   pwsh scripts\package_release.ps1
#   pwsh scripts\package_release.ps1 -Config Debug

[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')]
    [string] $Config = 'Release',

    [string] $OutputDir = 'release'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = Split-Path -Parent $PSScriptRoot
$outRoot  = Join-Path $repoRoot $OutputDir
if (Test-Path -LiteralPath $outRoot) {
    Remove-Item -LiteralPath $outRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $outRoot | Out-Null

function Copy-Artifact {
    param(
        [Parameter(Mandatory)] [string] $Arch
    )
    $isX64    = $Arch -eq 'x64'
    $ext      = if ($isX64) { 'dp64' } else { 'dp32' }
    $subDir   = if ($isX64) { 'x64' }  else { 'x32' }
    $buildDir = Join-Path $repoRoot "build-$Arch"
    $src      = Join-Path $buildDir "bin/$Config/x64dbg_ai_plugin.$ext"

    if (-not (Test-Path -LiteralPath $src)) {
        Write-Warning "[package] 跳过 $Arch：$src 不存在"
        return
    }

    $dstDir = Join-Path $outRoot "$subDir/plugins"
    New-Item -ItemType Directory -Path $dstDir -Force | Out-Null
    Copy-Item -LiteralPath $src -Destination $dstDir -Force
    Write-Host "[package] $Arch -> $dstDir" -ForegroundColor Green
}

Copy-Artifact -Arch x86
Copy-Artifact -Arch x64

# 生成 zip
$zipPath = Join-Path $repoRoot "x64dbg-ai-plugin-$Config.zip"
if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
Compress-Archive -Path (Join-Path $outRoot '*') -DestinationPath $zipPath
Write-Host "[package] zip: $zipPath" -ForegroundColor Cyan
