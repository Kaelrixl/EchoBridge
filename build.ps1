$ErrorActionPreference = 'Stop'
$projectDir = $PSScriptRoot
$buildDir = Join-Path $projectDir 'build'

cmake -S $projectDir -B $buildDir -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmake --build $buildDir --config Release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Native EXE: $(Join-Path $buildDir 'out/Release/EchoBridge.exe')"
