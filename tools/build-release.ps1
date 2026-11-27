$ErrorActionPreference = 'Stop'
Push-Location (Join-Path $PSScriptRoot '..')
try {
    $installed = Get-Command cmake.exe -ErrorAction SilentlyContinue
    $cmake = if ($installed) { $installed.Source } else { (Resolve-Path 'build/dependencies/cmake/bin/cmake.exe').Path }
    $ctest = Join-Path (Split-Path $cmake) 'ctest.exe'
    $build = 'build/release'
    $model = (Resolve-Path 'models/release/deepprint_texminu_512.onnx').Path
    & $cmake -S . -B $build -G 'MinGW Makefiles' -DCMAKE_BUILD_TYPE=Release `
        -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/g++.exe `
        -DCMAKE_MAKE_PROGRAM=C:/msys64/ucrt64/bin/mingw32-make.exe `
        -DFINGERPRINT_DEEPPRINT=ON -DFINGERPRINT_BUILD_GUI=ON -DBUILD_TESTING=ON `
        -DFINGERPRINT_FVC2004_ROI=ON '-DFINGERPRINT_TEXTURE_WEIGHT:STRING=0.22' `
        "-DFINGERPRINT_DEEPPRINT_MODEL:FILEPATH=$model" `
        -DFINGERPRINT_DEEPPRINT_REFERENCE_DIR=tests/fixtures/deepprint
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $cmake --build $build --parallel 4
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $ctest --test-dir $build --output-on-failure --no-tests=error
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $cmake --install $build --prefix dist
    exit $LASTEXITCODE
} finally {
    Pop-Location
}
