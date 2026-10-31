# Dot-source: sets up Rust (buildtools\rustup, buildtools\cargo), llvm-mingw (host linker/dlltool), git, wabt.
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bt = Join-Path $root 'buildtools'
$env:RUSTUP_HOME = "$bt\rustup"
$env:CARGO_HOME = "$bt\cargo"
$env:JAVA_HOME = "$bt\jre\jdk-21.0.12.1+1-jre"
$env:PATH = "$env:JAVA_HOME\bin;$bt\cargo\bin;$bt\llvm-mingw-20261006-ucrt-x86_64\bin;$bt\mingit\cmd;$bt\wabt-build;$bt\wabt\wabt-1.0.42\bin;$bt\cmake-4.4.4-windows-x86_64\bin;$bt\ninja;$env:PATH"
$RW = Join-Path $root 'ruffle-wiiu'
# Official LLVM release: clang with the WebAssembly target for C code in crates (jpegxr's jxrlib) and
# libclang for their bindgen step. llvm-mingw above has neither.
$llvm = "$bt\clang+llvm-23.1.3-x86_64-pc-windows-msvc\bin"
$env:CC_wasm32_wasip1 = "$llvm\clang.exe"
$env:AR_wasm32_wasip1 = "$llvm\llvm-ar.exe"
$env:LIBCLANG_PATH = $llvm
