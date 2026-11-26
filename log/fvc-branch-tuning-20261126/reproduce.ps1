param([string]$Output = 'build/fvc-branch-tuning-reproduction')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$cmake = Get-Command cmake.exe -ErrorAction SilentlyContinue
$cmakePath = if ($cmake) { $cmake.Source } else { Join-Path $root 'build/dependencies/cmake/bin/cmake.exe' }
& $cmakePath '-DSTUDY=branches' "-DOUTPUT=$Output" -P (Join-Path $root 'tools/deepprint/replay.cmake')
exit $LASTEXITCODE
