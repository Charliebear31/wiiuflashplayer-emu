# Shared step of build-pc.ps1 / build-wiiu.ps1: Ruffle (wasm32-wasip1) -> ruffle-wiiu\gen\*.c with wasm2c.
# Usage: . .\gen.ps1 [-Debug]
param([switch]$Debug)
. "$PSScriptRoot\env.ps1"

$profile = if ($Debug) { 'debug' } else { 'release' }
Push-Location "$RW\ruffle"
try {
    $cargoArgs = @('build', '-p', 'ruffle_wiiu', '--target', 'wasm32-wasip1')
    if (-not $Debug) { $cargoArgs += '--release' }
    cargo @cargoArgs 2>&1 | Where-Object { "$_" -match 'error|^warning: unused|Finished' } | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0) { throw "cargo failed" }
} finally { Pop-Location }

$wasm = "$RW\ruffle\target\wasm32-wasip1\$profile\ruffle_wiiu.wasm"
$gen = "$RW\gen"
$stamp = "$gen\flashplayer.h"
if (-not (Test-Path $stamp) -or (Get-Item $wasm).LastWriteTime -gt (Get-Item $stamp).LastWriteTime) {
    if (-not (Test-Path $gen)) { New-Item -ItemType Directory $gen | Out-Null }
    wasm2c $wasm -n flashplayer --num-outputs 16 -o "$gen\flashplayer.c"
    if ($LASTEXITCODE -ne 0) { throw "wasm2c failed" }
    "wasm2c: regenerated C"
}

# Wii U: wasm allows unaligned f32/f64 accesses, and wasm2c's helpers memcpy straight into a float, which
# GCC turns into lfs/lfd/stfs/stfd. Espresso raises an alignment exception for a float access that isn't
# word-aligned (integer loads are fine; Cemu doesn't check), so the console crashed loading playerglobal.
# The helpers below move the bits through integer registers instead.
$impl = "$gen\flashplayer-impl.h"
$text = [IO.File]::ReadAllText($impl)
if (-not $text.Contains('DEFINE_FLOAT_LOAD')) {
    $floatOps = @'
// ruffle-wiiu (tools/gen.ps1): float loads/stores through integer registers, never lfs/lfd/stfs/stfd on
// a possibly unaligned address (an alignment exception on the Wii U's CPU).
#define DEFINE_FLOAT_LOAD(name, t, ti)                                          \
  static inline t name##_unchecked(uint8_t* const wasm_rt_local_memory_base,   \
                                   wasm_rt_memory_t* mem, u64 addr) {          \
    ti bits;                                                                   \
    wasm_rt_memcpy(&bits, MEM_ADDR_MEMOP(mem, addr, sizeof(ti)), sizeof(ti));  \
    __asm__("" : "+r"(bits));                                                  \
    t ret;                                                                     \
    memcpy(&ret, &bits, sizeof(t));                                            \
    FORCE_READ_FLOAT(ret);                                                     \
    return ret;                                                                \
  }                                                                            \
  DEF_MEM_CHECKS0(name, _, t, return, t)
#define DEFINE_FLOAT_STORE(name, t, ti)                                         \
  static inline void name##_unchecked(uint8_t* const wasm_rt_local_memory_base, \
                                      wasm_rt_memory_t* mem, u64 addr, t value) { \
    ti bits;                                                                   \
    memcpy(&bits, &value, sizeof(t));                                          \
    __asm__("" : "+r"(bits));                                                  \
    wasm_rt_memcpy(MEM_ADDR_MEMOP(mem, addr, sizeof(ti)), &bits, sizeof(ti));  \
  }                                                                            \
  DEF_MEM_CHECKS1(name, _, t, , void, t)
DEFINE_FLOAT_LOAD(f32_load, f32, u32)
DEFINE_FLOAT_LOAD(f64_load, f64, u64)
'@
    $replaced = $text.Replace("DEFINE_LOAD(f32_load, f32, f32, f32, FORCE_READ_FLOAT)`n", '').Replace(
        "DEFINE_LOAD(f64_load, f64, f64, f64, FORCE_READ_FLOAT)`n", $floatOps).Replace(
        "DEFINE_STORE(f32_store, f32, f32)`n", '').Replace(
        "DEFINE_STORE(f64_store, f64, f64)`n", "DEFINE_FLOAT_STORE(f32_store, f32, u32)`nDEFINE_FLOAT_STORE(f64_store, f64, u64)`n")
    if ($replaced -eq $text -or ([regex]::Matches($replaced, 'DEFINE_FLOAT_(LOAD|STORE)\((f32|f64)_(load|store)')).Count -ne 4) {
        throw "gen.ps1: couldn't patch the float helpers in $impl (wasm2c output changed?)"
    }
    [IO.File]::WriteAllText($impl, $replaced, (New-Object Text.UTF8Encoding $false))
    "patched float loads/stores for the Wii U"
}

# Wii U speed: with RW_UNCHECKED_MEMORY (set in host/wiiu/CMakeLists.txt) wasm memory accesses skip their
# bounds checks and the small load/store helpers are always inlined. The generated code is built with
# -fno-inline to fit the 32 MB code limit, which made every access a call plus a check; without the check
# an inlined access is about as small as the call. Ruffle is memory-safe Rust, so the checks only caught
# miscompiles; a wild access still ends in the crash log (DSI).
$text = [IO.File]::ReadAllText($impl)
if (-not $text.Contains('RW_UNCHECKED_MEMORY')) {
    $marker = "// When using guard pages, reads have to be immediately consumed"
    $unchecked = @'
// ruffle-wiiu (tools/gen.ps1): no bounds checks, inlined accessors (see gen.ps1).
#if RW_UNCHECKED_MEMORY
#undef MEMCHECK_DEFAULT32
#define MEMCHECK_DEFAULT32(mem, local_memory_size, a, t)
#undef MEMCHECK_GENERAL
#define MEMCHECK_GENERAL(mem, a, t)
#define RW_INLINE __attribute__((always_inline))
#else
#define RW_INLINE
#endif

'@
    $start = $text.IndexOf('#define DEF_MEM_CHECKS0')
    $end = $text.IndexOf('DEFINE_STORE(i64_store32, u32, u64)')
    if ($text.IndexOf($marker) -lt 0 -or $start -lt 0 -or $end -lt $start) {
        throw "gen.ps1: couldn't patch the memory checks in $impl (wasm2c output changed?)"
    }
    $accessors = $text.Substring($start, $end - $start).Replace('static inline ', 'static inline RW_INLINE ')
    $text = $text.Substring(0, $start) + $accessors + $text.Substring($end)
    $text = $text.Replace($marker, $unchecked + $marker)
    [IO.File]::WriteAllText($impl, $text, (New-Object Text.UTF8Encoding $false))
    "patched memory checks / accessor inlining for the Wii U"
}

# Wii U: with the accessors inlined, GCC copied 64-bit values from wasm memory to wasm memory through a
# float register (lfd/stfd), which raises an alignment exception on the console when the address isn't
# word-aligned (it crashed loading Digger). Every value loaded or stored now passes through an empty asm
# that wants it in integer registers, so wasm memory is only ever touched with integer loads/stores.
$text = [IO.File]::ReadAllText($impl)
if (-not $text.Contains('RW_GPR(')) {
    $loadLine = '    t3 ret = (t3)(t2)result;'
    $storeLine = '    t1 wrapped = (t1)value;'
    $defLoad = '#define DEFINE_LOAD(name, t1, t2, t3, force_read)'
    if (-not ($text.Contains($loadLine) -and $text.Contains($storeLine) -and $text.Contains($defLoad))) {
        throw "gen.ps1: couldn't add the integer-register barrier to $impl (wasm2c output changed?)"
    }
    $text = $text.Replace($loadLine, '    RW_GPR(result); t3 ret = (t3)(t2)result;')
    $text = $text.Replace($storeLine, '    t1 wrapped = (t1)value; RW_GPR(wrapped);')
    $text = $text.Replace($defLoad, "// ruffle-wiiu (tools/gen.ps1): keep a value in integer registers (see gen.ps1).`n#define RW_GPR(v) __asm__(`"`" : `"+r`"(v))`n" + $defLoad)
    [IO.File]::WriteAllText($impl, $text, (New-Object Text.UTF8Encoding $false))
    "patched integer loads/stores to stay in integer registers"
}

# Even so, GCC turned the 8-byte memcpy itself into lfd/stfd (into a stack slot, then into registers).
# All wasm memory copies now go through rw_copy, which moves 8 bytes as two 4-byte integer words kept
# in integer registers. Bytes are copied as they are, so it works for either memory byte order.
$text = [IO.File]::ReadAllText($impl)
if (-not $text.Contains('rw_copy(')) {
    $copyDef = @'
// ruffle-wiiu (tools/gen.ps1): wasm memory copies without float registers (see gen.ps1).
static inline __attribute__((always_inline)) void rw_copy(void* d, const void* s, size_t n) {
  if (n == 8) {
    u32 a, b;
    memcpy(&a, s, 4);
    memcpy(&b, (const u8*)s + 4, 4);
    RW_GPR(a);
    RW_GPR(b);
    memcpy(d, &a, 4);
    memcpy((u8*)d + 4, &b, 4);
  } else {
    memcpy(d, s, n);
  }
}
'@
    $pairs = @(
        @('wasm_rt_memcpy(&result, MEM_ADDR_MEMOP(mem, addr, sizeof(t1)),', 'rw_copy(&result, MEM_ADDR_MEMOP(mem, addr, sizeof(t1)),'),
        @('wasm_rt_memcpy(MEM_ADDR_MEMOP(mem, addr, sizeof(t1)), &wrapped,', 'rw_copy(MEM_ADDR_MEMOP(mem, addr, sizeof(t1)), &wrapped,'),
        @('wasm_rt_memcpy(&bits, MEM_ADDR_MEMOP(mem, addr, sizeof(ti)), sizeof(ti));', 'rw_copy(&bits, MEM_ADDR_MEMOP(mem, addr, sizeof(ti)), sizeof(ti));'),
        @('wasm_rt_memcpy(MEM_ADDR_MEMOP(mem, addr, sizeof(ti)), &bits, sizeof(ti));', 'rw_copy(MEM_ADDR_MEMOP(mem, addr, sizeof(ti)), &bits, sizeof(ti));')
    )
    foreach ($p in $pairs) {
        if (-not $text.Contains($p[0])) { throw "gen.ps1: couldn't find '$($p[0])' in $impl (wasm2c output changed?)" }
        $text = $text.Replace($p[0], $p[1])
    }
    $gprDef = '#define RW_GPR(v) __asm__("" : "+r"(v))'
    $text = $text.Replace($gprDef, $gprDef + "`n" + $copyDef.TrimEnd())
    if (-not $text.Contains('static inline __attribute__((always_inline)) void rw_copy')) { throw "gen.ps1: couldn't add rw_copy to $impl" }
    [IO.File]::WriteAllText($impl, $text, (New-Object Text.UTF8Encoding $false))
    "patched wasm memory copies to use integer words"
}
