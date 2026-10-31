# Builds Ruffle (wasm32-wasip1) -> wasm2c C -> PC test host ruffle-wiiu\build-pc\rwpc.exe.
# Usage: .\build-pc.ps1 [-Debug] (Debug builds the Rust side without optimizations)
param([switch]$Debug)
. "$PSScriptRoot\gen.ps1" -Debug:$Debug

$build = "$RW\build-pc"
$sdl = ("$RW\..\buildtools\sdl2-mingw\SDL2-2.32.10\x86_64-w64-mingw32" | Resolve-Path).Path -replace '\\', '/'
if (-not (Test-Path "$build\build.ninja")) {
    cmake -S "$RW\host\pc" -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang "-DSDL2_DIR=$sdl" | Out-Null
}
cmake --build $build 2>&1 | Where-Object { "$_" -match 'error|warning: implicit|FAILED|Linking' } | ForEach-Object { "$_" }
if ($LASTEXITCODE -ne 0) { throw "C build failed" }
Copy-Item "$sdl\bin\SDL2.dll" $build -Force
"built $build\rwpc.exe"
