# scripts/package_release.ps1
#
# 把 build-x86 / build-x64 的插件产物 + Qt 运行依赖（如有）打包到
# release/ 目录，并生成 zip。
#
# 用法：
#   pwsh scripts\package_release.ps1
#   pwsh scripts\package_release.ps1 -Config Debug
#   pwsh scripts\package_release.ps1 -Version 0.1.1

[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')]
    [string] $Config = 'Release',

    [string] $Version = '',

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

$x86Artifact = Join-Path $repoRoot "build-x86/bin/$Config/x64dbg_ai_plugin.dp32"
$x64Artifact = Join-Path $repoRoot "build-x64/bin/$Config/x64dbg_ai_plugin.dp64"
if (-not (Test-Path -LiteralPath $x86Artifact) -or -not (Test-Path -LiteralPath $x64Artifact)) {
    throw "缺少双架构构建产物，请先构建 x86 与 x64：$x86Artifact / $x64Artifact"
}

Copy-Artifact -Arch x86
Copy-Artifact -Arch x64

# 生成 zip；发布时传入版本号，生成可直接上传到 GitHub Release 的版本化资产。
$archiveName = if ([string]::IsNullOrWhiteSpace($Version)) {
    "x64dbg-ai-plugin-$Config.zip"
} else {
    "x64dbg-ai-plugin-v$Version.zip"
}
$zipPath = Join-Path $repoRoot $archiveName
if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
Compress-Archive -Path (Join-Path $outRoot '*') -DestinationPath $zipPath
Write-Host "[package] zip: $zipPath" -ForegroundColor Cyan
