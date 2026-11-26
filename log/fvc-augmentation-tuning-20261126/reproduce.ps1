param([string]$Output = 'build/fvc-augmentation-tuning-reproduction')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$cmake = Get-Command cmake.exe -ErrorAction SilentlyContinue
$cmakePath = if ($cmake) { $cmake.Source } else { Join-Path $root 'build/dependencies/cmake/bin/cmake.exe' }
& $cmakePath '-DSTUDY=augmentation' "-DOUTPUT=$Output" -P (Join-Path $root 'tools/deepprint/replay.cmake')
exit $LASTEXITCODE
