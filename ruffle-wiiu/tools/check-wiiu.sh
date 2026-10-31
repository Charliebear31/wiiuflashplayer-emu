#!/bin/bash
# Compile-only check of the Wii U host files with devkitPPC (no shaders or packaging needed).
# Usage (msys bash): tools/check-wiiu.sh [gen file to also time, e.g. ruffle_15.c]
source /etc/profile.d/devkit-env.sh
cd "$(dirname "$0")/../host" || exit 1
out=/tmp/rwchk
mkdir -p $out
FL="-mcpu=750 -meabi -mhard-float -O2 -D__WIIU__ -D__WUT__ -DWABT_BIG_ENDIAN=1 -DWASM_RT_USE_MMAP=0 \
 -DWASM_RT_MEMCHECK_BOUNDS_CHECK=1 -DWASM_RT_NO_THREAD_LOCAL=1 -DWASM_RT_MAX_CALL_STACK_DEPTH=20000 \
 -I. -Iwasm-rt -I../gen -I$DEVKITPRO/wut/include -I$DEVKITPRO/portlibs/wiiu/include/SDL2 \
 -I$DEVKITPRO/portlibs/ppc/include -Wall -Wno-unused-function"
for f in rw_core.c wiiu/gfx_gx2.c wiiu/main_wiiu.c wasm-rt/wasm-rt-impl.c wasm-rt/wasm-rt-mem-impl.c wasm-rt/wasm-rt-exceptions-impl.c; do
  echo "== $f"
  $DEVKITPPC/bin/powerpc-eabi-gcc $FL -c $f -o $out/$(basename $f).o 2>&1 | grep -E 'error|warning' | head -20
done
if [ -n "$1" ]; then
  echo "== timing ../gen/$1"
  time $DEVKITPPC/bin/powerpc-eabi-gcc $FL -O1 -w -c ../gen/$1 -o $out/gen.o
fi
ls -la $out
