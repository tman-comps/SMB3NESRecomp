# Configure + build (Ninja + clang).
#   .\build.ps1                       # Release, windowed
#   .\build.ps1 -Headless             # no SDL window (smoke tests)
#   .\build.ps1 -Rom path\to\smb3.nes # explicit ROM path
# If a portable toolchain sits next to the repo (..\DevTools\env.ps1), it is
# put on PATH first; otherwise the tools must already be on PATH.
param(
    [string]$Rom = "",
    [string]$BuildDir = "build",
    [switch]$Headless
)
$ErrorActionPreference = 'Stop'
$toolchain = Join-Path $PSScriptRoot '..\DevTools\env.ps1'
if (Test-Path $toolchain) { . $toolchain }
Set-Location $PSScriptRoot

$cfg = @('-S', '.', '-B', $BuildDir, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
         '-DCMAKE_C_COMPILER=clang', '-DCMAKE_CXX_COMPILER=clang++')
if ($Rom) { $cfg += "-DNESRECOMP_ROM=$Rom" }
if ($Headless) { $cfg += '-DNESRECOMP_HEADLESS=ON' }

cmake @cfg
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmake --build $BuildDir --parallel
exit $LASTEXITCODE
