param(
  [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
  [string]$Config = 'Debug',
  [string]$BuildDir = '',
  [string]$VcpkgRoot = 'C:\SDK\vcpkg\vcpkg',
  [string]$VcpkgTriplet = 'x64-windows-static-md',
  [int]$Parallel = 0,
  [switch]$CleanConfigure,
  [switch]$RunSmokeTest
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
  $BuildDir = Join-Path $repo ("build-qt-$($Config.ToLowerInvariant())")
}

$prefix = Join-Path (Join-Path (Join-Path $VcpkgRoot 'installed') $VcpkgTriplet) ''
if (-not (Test-Path (Join-Path $prefix 'include\zlib.h'))) {
  throw "zlib.h not found under $prefix. Set -VcpkgRoot/-VcpkgTriplet to the installed vcpkg triplet."
}
if (-not (Test-Path (Join-Path $prefix 'share\zlib\ZLIBConfig.cmake'))) {
  throw "ZLIBConfig.cmake not found under $prefix. Install zlib for the selected triplet."
}

if ($CleanConfigure -and (Test-Path $BuildDir)) {
  Remove-Item -LiteralPath $BuildDir -Recurse -Force
}

$cmakeArgs = @(
  '-S', $repo,
  '-B', $BuildDir,
  '-DWDS_USE_QT_EDITOR=ON',
  "-DCMAKE_PREFIX_PATH=$prefix",
  "-DWDS_VCPKG_ROOT=$VcpkgRoot",
  "-DWDS_VCPKG_TRIPLET=$VcpkgTriplet"
)

Write-Host "Configuring Qt editor in $BuildDir"
& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$buildArgs = @('--build', $BuildDir, '--config', $Config, '--target', 'wds_editor')
if ($Parallel -gt 0) { $buildArgs += @('--parallel', $Parallel) }
Write-Host "Building wds_editor ($Config)"
& cmake @buildArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($RunSmokeTest) {
  $exe = Join-Path $BuildDir "ui\$Config\wds_editor.exe"
  if (-not (Test-Path $exe)) { throw "Built executable not found: $exe" }
  & $exe '--smoke-test'
  exit $LASTEXITCODE
}

Write-Host "Build succeeded. Executable: $(Join-Path $BuildDir "ui\$Config\wds_editor.exe")"
