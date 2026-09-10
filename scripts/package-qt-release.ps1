param(
  [ValidateSet('Release', 'RelWithDebInfo')]
  [string]$Config = 'Release',
  [string]$BuildDir = '',
  [string]$OutputDir = '',
  [string]$Windeployqt = '',
  [switch]$Build,
  [switch]$NoZip
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($BuildDir)) { $BuildDir = Join-Path $repo "build-qt-$($Config.ToLowerInvariant())" }
if ([string]::IsNullOrWhiteSpace($OutputDir)) { $OutputDir = Join-Path $repo "dist\wds-editor-$Config" }
$exe = Join-Path $BuildDir "ui\$Config\wds_editor.exe"

if ($Build) {
  & cmake --build $BuildDir --config $Config --target wds_editor --parallel
  if ($LASTEXITCODE -ne 0) { throw "wds_editor build failed" }
}
if (-not (Test-Path -LiteralPath $exe)) { throw "Executable not found: $exe. Build first or pass -BuildDir." }

if (Test-Path -LiteralPath $OutputDir) { Remove-Item -LiteralPath $OutputDir -Recurse -Force }
New-Item -ItemType Directory -Path $OutputDir | Out-Null
Copy-Item -LiteralPath $exe -Destination $OutputDir
$stagedExe = Join-Path $OutputDir 'wds_editor.exe'

if ([string]::IsNullOrWhiteSpace($Windeployqt)) {
  $deployCommand = Get-Command windeployqt.exe -ErrorAction SilentlyContinue
  if ($deployCommand) { $Windeployqt = $deployCommand.Source }
}
if ([string]::IsNullOrWhiteSpace($Windeployqt) -or
    -not (Test-Path -LiteralPath $Windeployqt)) {
  throw 'windeployqt.exe not found. Add Qt bin to PATH or pass -Windeployqt.'
}
# Deploy beside the staged executable, not back into the CMake build tree.
& $Windeployqt --release --no-translations --no-system-d3d-compiler --no-opengl-sw $stagedExe
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed" }

function Copy-Tree([string]$source, [string]$name) {
  if (-not (Test-Path -LiteralPath $source)) { throw "Required resource directory missing: $source" }
  Copy-Item -LiteralPath $source -Destination (Join-Path $OutputDir $name) -Recurse -Force
}
function Copy-FileIfPresent([string]$source, [string]$name) {
  if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination (Join-Path $OutputDir $name) -Force }
}

Copy-Tree (Join-Path $repo 'skins') 'skins'
Copy-Tree (Join-Path $repo 'effects') 'effects'
Copy-Tree (Join-Path $repo 'ui\assets\fonts') 'fonts'
Copy-Tree (Join-Path $repo 'ui\assets\theme') 'theme'
Copy-Tree (Join-Path $repo 'icons') 'icons'
Copy-Tree (Join-Path $BuildDir 'renderer\shaders') 'shaders'
Copy-FileIfPresent (Join-Path $repo 'ui\assets\app_icon\wds.png') 'wds.png'
Copy-FileIfPresent (Join-Path $repo 'audio-player\third_party\bass\win-x86_64\bass.dll') 'bass.dll'
Copy-FileIfPresent (Join-Path $repo 'audio-player\third_party\bass\win-x86_64\bassmix.dll') 'bassmix.dll'

# Vulkan loader is optional because current Windows GPU drivers normally ship it.
$vulkanCandidates = @()
if ($env:VULKAN_SDK) {
  $vulkanCandidates += Join-Path $env:VULKAN_SDK 'Bin\vulkan-1.dll'
}
foreach ($candidate in $vulkanCandidates) {
  if ($candidate -and (Test-Path -LiteralPath $candidate)) { Copy-Item -LiteralPath $candidate -Destination (Join-Path $OutputDir 'vulkan-1.dll') -Force; break }
}

@"
WDS Editor $((Get-Item $exe).VersionInfo.ProductVersion)

Portable Windows package. Keep wds_editor.exe beside the folders and DLLs.
Resources: skins, effects, fonts, theme, icons, shaders.
"@ | Set-Content -LiteralPath (Join-Path $OutputDir 'README.txt') -Encoding UTF8

if (-not $NoZip) {
  $zip = "$OutputDir.zip"
  if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
  Compress-Archive -Path (Join-Path $OutputDir '*') -DestinationPath $zip -CompressionLevel Optimal
  Write-Host "Packaged: $zip"
}
Write-Host "Package directory: $OutputDir"
