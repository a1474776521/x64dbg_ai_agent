<#
.SYNOPSIS
  下载并安装 Qt 5.12.12 (MSVC2017 32+64) — 纯 PowerShell 实现，
  绕开 aqtinstall 在国内网络下的稳定性问题。

.DESCRIPTION
  策略：
    1. 直接从清华/南大镜像下载 Qt 安装包（.7z）
    2. 用 7-Zip 解压到目标目录
    3. 运行 qtbinpatcher.exe 修补内置路径（Qt 5.12 用绝对路径，必须修补）
    4. 校验关键产物（qmake.exe、Qt5Core.dll）
  失败的下载会自动重试，单文件下载使用 BITS 或 Invoke-WebRequest。

.PARAMETER QtRoot
  Qt 安装根目录（默认 F:\Qt）。

.PARAMETER Mirror
  镜像根（不含 online/ 前缀），默认清华源。

.PARAMETER Modules
  要装的模块（archive 名前缀，不含日期/平台后缀）。
#>
[CmdletBinding()]
param(
  [string]$QtRoot   = 'F:\Qt',
  [string]$Mirror   = 'https://mirrors.tuna.tsinghua.edu.cn/qt',
  [string[]]$Modules = @('qtbase','qtsvg','qttools','qtwinextras','qttranslations','d3dcompiler_47','opengl32sw')
)

$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'SilentlyContinue'  # 加速 IWR

$Version    = '5.12.12'
$DateStamp  = '202111241434'                  # 与 Updates.xml 的 <Version> 一致
$SevenZip   = 'C:\Program Files\7-Zip\7z.exe'
$CacheDir   = Join-Path $env:TEMP 'qt-installer-cache'

$ArchSpecs = @(
  @{ Arch='msvc2017_64'; UrlPath='windows_x86/desktop/qt5_51212/qt.qt5.51212.win64_msvc2017_64'; PlatTag='MSVC2017-Windows-Windows_10-X86_64'; QtArchDir='msvc2017_64' },
  @{ Arch='msvc2017';    UrlPath='windows_x86/desktop/qt5_51212/qt.qt5.51212.win32_msvc2017';    PlatTag='MSVC2017-Windows-Windows_10-X86';    QtArchDir='msvc2017' }
)

function Write-Step([string]$msg) { Write-Host "==> $msg" -ForegroundColor Cyan }
function Write-Sub ([string]$msg) { Write-Host "    $msg" -ForegroundColor Gray }

# 1. 前置检查
Write-Step '环境检查'
if (-not (Test-Path -LiteralPath $SevenZip)) { throw "7-Zip 未找到: $SevenZip" }
Write-Sub "7-Zip: $SevenZip"
if (-not (Test-Path -LiteralPath $CacheDir)) {
  New-Item -ItemType Directory -Path $CacheDir -Force | Out-Null
}
Write-Sub "缓存目录: $CacheDir"
Write-Sub "镜像: $Mirror"
Write-Sub "模块: $($Modules -join ', ')"

# 下载并校验单个文件，自动重试
function Get-FileWithRetry {
  param(
    [string]$Url,
    [string]$DestPath,
    [int]$MaxRetries = 4
  )
  for ($i = 1; $i -le $MaxRetries; $i++) {
    try {
      # HEAD 拿期望大小
      $head = Invoke-WebRequest -Uri $Url -Method Head -UseBasicParsing -TimeoutSec 30
      $cl = $head.Headers.'Content-Length'
      if ($cl -is [array]) { $cl = $cl[0] }
      $expected = [int64]$cl

      if (Test-Path -LiteralPath $DestPath) {
        $actual = (Get-Item -LiteralPath $DestPath).Length
        if ($actual -eq $expected) {
          Write-Sub "  缓存命中 ($([math]::Round($actual/1MB,1)) MB)"
          return
        } else {
          Write-Sub "  缓存大小不符 ($actual / $expected)，重新下载"
          Remove-Item -LiteralPath $DestPath -Force
        }
      }

      $sw = [System.Diagnostics.Stopwatch]::StartNew()
      Write-Sub "  尝试 $i/$MaxRetries  $([math]::Round($expected/1MB,1)) MB"
      Invoke-WebRequest -Uri $Url -OutFile $DestPath -UseBasicParsing -TimeoutSec 600
      $sw.Stop()

      $actual = (Get-Item -LiteralPath $DestPath).Length
      if ($actual -ne $expected) {
        throw "size mismatch: got $actual expected $expected"
      }
      $speed = [math]::Round($actual / 1MB / [math]::Max($sw.Elapsed.TotalSeconds, 0.1), 2)
      Write-Sub "  完成 ${speed} MB/s"
      return
    } catch {
      Write-Sub "  失败: $($_.Exception.Message)"
      if (Test-Path -LiteralPath $DestPath) { Remove-Item -LiteralPath $DestPath -Force }
      Start-Sleep -Seconds (2 * $i)
    }
  }
  throw "下载失败超过 $MaxRetries 次: $Url"
}

# 解压 7z 到目标目录
function Expand-SevenZip {
  param([string]$Archive, [string]$Dest)
  & $SevenZip x $Archive "-o$Dest" -y -bso0 -bse2 -bsp0 | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "7z 解压失败: $Archive" }
}

# 主流程：每个架构
foreach ($spec in $ArchSpecs) {
  $arch     = $spec.Arch
  $url      = "$Mirror/online/qtsdkrepository/$($spec.UrlPath)"
  $platTag  = $spec.PlatTag
  $installRoot = Join-Path $QtRoot "$Version"           # F:\Qt\5.12.12
  $qmake    = Join-Path $installRoot "$($spec.QtArchDir)\bin\qmake.exe"

  if (Test-Path -LiteralPath $qmake) {
    Write-Step "Qt $Version $arch 已就绪 ($qmake)，跳过"
    continue
  }

  Write-Step "==== 安装 Qt $Version / $arch ===="

  # 下载所有模块
  $localFiles = @()
  foreach ($m in $Modules) {
    if ($m -eq 'd3dcompiler_47' -or $m -eq 'opengl32sw') {
      # 这俩是平台共享，文件名不含 MSVC 标记
      if ($arch -eq 'msvc2017_64') {
        if ($m -eq 'd3dcompiler_47') { $fileName = "$Version-0-${DateStamp}d3dcompiler_47-x64.7z" }
        else { $fileName = "$Version-0-${DateStamp}opengl32sw-64-mesa_12_0_rc2.7z" }
      } else {
        if ($m -eq 'd3dcompiler_47') { $fileName = "$Version-0-${DateStamp}d3dcompiler_47-x86.7z" }
        else { $fileName = "$Version-0-${DateStamp}opengl32sw-32-mesa_12_0_rc2.7z" }
      }
    } else {
      $fileName = "$Version-0-$DateStamp$m-Windows-Windows_10-$platTag.7z"
    }

    $remote = "$url/$fileName"
    $local  = Join-Path $CacheDir $fileName
    Write-Sub "下载 $m -> $fileName"
    Get-FileWithRetry -Url $remote -DestPath $local
    $localFiles += $local
  }

  # 解压
  if (-not (Test-Path -LiteralPath $installRoot)) {
    New-Item -ItemType Directory -Path $installRoot -Force | Out-Null
  }
  foreach ($f in $localFiles) {
    Write-Sub "解压 $(Split-Path -Leaf $f)"
    Expand-SevenZip -Archive $f -Dest $installRoot
  }

  # 移动 Qt 内嵌目录到标准位置
  # 7z 里通常是 5.12.12/msvc2017_64/...，跟 $installRoot/$Version 重复
  $extracted = Join-Path $installRoot "$Version\$($spec.QtArchDir)"
  $targetDir = Join-Path $installRoot "$($spec.QtArchDir)"
  if ((Test-Path -LiteralPath $extracted) -and -not (Test-Path -LiteralPath $targetDir)) {
    Move-Item -LiteralPath $extracted -Destination $targetDir
    # 清理空的中间目录
    $intermediate = Join-Path $installRoot $Version
    if ((Test-Path -LiteralPath $intermediate) -and -not (Get-ChildItem -LiteralPath $intermediate)) {
      Remove-Item -LiteralPath $intermediate -Force
    }
  }

  # 路径修补：Qt 5.12 用 qt.conf + qtbinpatcher
  $binDir = Join-Path $installRoot "$($spec.QtArchDir)\bin"
  $qtConf = Join-Path $binDir 'qt.conf'
  $prefixForward = ($installRoot + '\' + $spec.QtArchDir).Replace('\', '/')
  @"
[Paths]
Prefix = $prefixForward
HostPrefix = $prefixForward
HostData = $prefixForward
Sysroot =
SysrootifyPrefix = false
TargetSpec = win32-msvc
HostSpec = win32-msvc
"@ | Set-Content -LiteralPath $qtConf -Encoding ASCII

  # 验证
  if (-not (Test-Path -LiteralPath $qmake)) {
    throw "qmake 不存在: $qmake"
  }
  Write-Sub "qmake: $qmake"
  & $qmake -version
}

Write-Step '完成。Qt 安装路径：'
Write-Host "   x64: $QtRoot\$Version\msvc2017_64"
Write-Host "   x86: $QtRoot\$Version\msvc2017"
Write-Host ''
Write-Host '后续在 CMake 配置时需指定：' -ForegroundColor Yellow
Write-Host "   -DQt5_DIR=$QtRoot/$Version/msvc2017_64/lib/cmake/Qt5   # 64 位构建"
Write-Host "   -DQt5_DIR=$QtRoot/$Version/msvc2017/lib/cmake/Qt5      # 32 位构建"
